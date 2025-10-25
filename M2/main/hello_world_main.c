#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/touch_pad.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define TAG "ESTEIRA"

// ===== Configuração de rede =====
#define WIFI_SSID "Nome do Wifi"
#define WIFI_PASS "Senha do Wifi"
#define TCP_PORT 10420 // qualquer número entre 0 e 65535
#define PC_IP 	"IP do ipconfig"
#define UDP_PORT 10421 // qualquer número entre 0 e 65535
#define MAX_PAYLOAD_SIZE 1024 // Define um tamanho máximo para a mensagem de rede

// ===== Mapas dos touch pads =====
// TP_OBJ -> Touch B: detecta objeto (para acionar desviador)
// TP_HMI -> Touch C: solicita atualização de HMI/telemetria
// TP_ESTOP -> Touch D: E-stop (emergência)
#define TP_OBJ 	TOUCH_PAD_NUM0 	// GPIO4
#define TP_HMI 	TOUCH_PAD_NUM3 	// GPIO15
#define TP_ESTOP TOUCH_PAD_NUM7 	// GPIO27

// ===== Configurações de prioridade e stack =====
#define ENC_T_MS 	5 	 	// período task enc_sense (ms)
#define PRIO_ESTOP 5
#define PRIO_ENC 	4
#define PRIO_CTRL 	3
#define PRIO_SORT 	2
#define PRIO_REP 	1 	 	// task de relatório
#define PRIO_TIME 	1
#define PRIO_TCP 	5
#define PRIO_UDP 	5
#define STK 	 	4096 	// tamanho de stack (bytes)

// ===== Handles das tasks, filas e semáforos =====
static TaskHandle_t hENC = NULL, hCTRL = NULL, hSORT = NULL, hSAFE = NULL, hREP = NULL;
static TaskHandle_t hCtrlNotify = NULL;
static QueueHandle_t qSort = NULL; 	 	// fila de eventos SORT_ACT
static QueueHandle_t qSafety = NULL; 	// fila de eventos SAFETY
static QueueHandle_t qHMI = NULL; // 	Fila para Latência HMI
static SemaphoreHandle_t semEStop = NULL; // semáforo para E-stop
static SemaphoreHandle_t mutexBeltState = NULL; // Protege g_belt e estop_ativo
static SemaphoreHandle_t mutexStats = NULL; 	 	// Protege stats e cpu_time

static volatile bool estop_ativo = false; // flag de E-stop ativo

// ===== Estado da esteira =====
typedef struct {
	float rpm; 	 	// velocidade atual (rpm)
	float pos_mm; 	// posição em mm
	float set_rpm; 	// setpoint de rpm
} belt_state_t;

static belt_state_t g_belt = {0.f, 0.f, 120.f};

// Estruturas para eventos
typedef struct {
	int64_t t_evt_us; // timestamp do evento (para latência)
	int pad; 	 	// qual touch pad
} sort_evt_t;

typedef struct {
	int64_t t_evt_us;
	int pad;
} safety_evt_t;

// ===== Estatísticas =====
static struct {
	uint32_t enc_samples, enc_misses;
	uint32_t ctrl_runs, ctrl_misses;
	uint32_t sort_events, sort_misses;
	uint32_t safety_events, safety_misses;
} last_stats = {0};

// Métricas acumuladas
static struct {
	uint32_t enc_samples, enc_misses;
	uint32_t ctrl_runs, ctrl_misses;
	uint32_t sort_events, sort_misses;
	uint32_t safety_events, safety_misses;

	int64_t sort_lat_total_us; 	// soma latência SORT
	uint32_t sort_lat_count; 	 	// qtd de eventos medidos

	int64_t safety_lat_total_us; // soma latência Safety
	uint32_t safety_lat_count;

	int64_t hmi_lat_total_us; 	 	// soma latência HMI
	uint32_t hmi_lat_count;
} stats = {0};

// Contadores de tempo ocupado da CPU (microsegundos)
static struct {
	int64_t enc_time_us;
	int64_t ctrl_time_us;
	int64_t sort_time_us;
	int64_t safety_time_us;
} cpu_time = {0};

void activate_estop(void);
void set_belt_setpoint(float rpm);
void reset_estop(void);
void get_belt_state(belt_state_t *state);
static void process_command(const char *command_rx, char *response_tx, size_t max_len);

// ===== Função busy loop determinístico =====
static inline void cpu_tight_loop_us(uint32_t us)
{
	int64_t start = esp_timer_get_time();
	while ((esp_timer_get_time() - start) < (int64_t)us) {
		asm volatile("nop");
	}
}

void set_belt_setpoint(float rpm) {
	if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
		g_belt.set_rpm = rpm; 
		xSemaphoreGive(mutexBeltState);
	}
}

void get_belt_state(belt_state_t *state) {
    if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
        memcpy(state, &g_belt, sizeof(belt_state_t));
        xSemaphoreGive(mutexBeltState);
    } else {
        memset(state, 0, sizeof(belt_state_t));
    }
}

void activate_estop(void) {
	safety_evt_t ev = {.t_evt_us = esp_timer_get_time(), .pad = TP_ESTOP};
	if (xQueueSend(qSafety, &ev, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Comando E-STOP acionado (via cliente).");
    } else {
        ESP_LOGE(TAG, "Falha ao enviar evento de E-STOP para a fila!");
    }
}

void reset_estop(void) {
	// A função set_belt_setpoint já protege o acesso a g_belt.set_rpm
	set_belt_setpoint(120.f); 
	ESP_LOGI(TAG, "Comando E-STOP resetado (via cliente).");
    // A flag 'estop_ativo' é resetada pela task_spd_ctrl quando vê que o setpoint é > 0.
}

static void process_command(const char *command_rx, char *response_tx, size_t max_len) {
    float new_rpm = 0.0f;
    
    if (strstr(command_rx, "status")) {
        belt_state_t state;
        get_belt_state(&state);
        snprintf(response_tx, max_len, 
                 "{\"ok\":true,\"rpm\":%.1f,\"set_rpm\":%.1f,\"pos\":%.1f,\"estop_ativo\":%s}\n", 
                 state.rpm, state.set_rpm, state.pos_mm, estop_ativo ? "true" : "false");         
    } else if (strstr(command_rx, "estop_on")) {
        activate_estop();
        snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"estop_on\"}\n");
    } else if (strstr(command_rx, "estop_reset")) {
        reset_estop();
        snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"estop_reset\"}\n");
    } else if (sscanf(command_rx, "set_rpm %f", &new_rpm) == 1) {
         set_belt_setpoint(new_rpm);
         snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"set_rpm\",\"value\":%.1f}\n", new_rpm);
    } else {
        // Comando desconhecido ou mal formatado
        snprintf(response_tx, max_len, "{\"ok\":false,\"error\":\"Comando desconhecido ou mal formatado: %s\"}\n", command_rx);
    }
}

//Configura WiFi da ESP32
static void wifi_init(void) {
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	wifi_config_t wifi_config = {
		.sta = {
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
		},
	};
	strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
	strcpy((char*)wifi_config.sta.password, WIFI_PASS);
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_LOGI(TAG, "Conectando ao WiFi...");
	ESP_ERROR_CHECK(esp_wifi_connect());
}

//Apenas para Log
static void time_sync_notification_cb(struct timeval *tv){
	ESP_LOGI(TAG, "Tempo sincronizado via SNTP.");
}

static void task_time(void *arg)
{
	time_t now = 0;
	struct tm timeinfo = {0};

	// Configuração do Fuso Horário
	setenv("TZ", "GMT+3", 1);
	tzset();

	ESP_LOGI(TAG, "Iniciando SNTP...");
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_set_time_sync_notification_cb(time_sync_notification_cb);
	sntp_setservername(0, "a.st1.ntp.br");
	sntp_setservername(1, "b.st1.ntp.br");
	sntp_setservername(2, "pool.ntp.org");
	sntp_init();

	int max_retries = 30;
	for (int i = 0; i < max_retries && sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET; ++i) {
		vTaskDelay(pdMS_TO_TICKS(500));
	}

	// Exibe a hora inicial após o sync
	time(&now);
	localtime_r(&now, &timeinfo);
	char buf[64];
	strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);
	ESP_LOGI(TAG, "Hora inicial: %s", buf);
	
	while (1) {
		vTaskDelay(pdMS_TO_TICKS(10000));
		
		time(&now);
		localtime_r(&now, &timeinfo);
		char buf[64];
		strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);
		ESP_LOGI(TAG, "Hora atual: %s", buf);
	}
}

static void udp_server_task(void *arg) {
    // Cria o socket UDP (SOCK_DGRAM)
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket UDP.");
        vTaskDelete(NULL);
    }

    // Configura e vincula a porta (Bind)
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(UDP_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // Escuta em todas as interfaces

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Falha ao fazer bind do socket UDP.");
        close(sock);
        vTaskDelete(NULL);
    }
	ESP_LOGI(TAG, "Servidor UDP ativo na porta %d", UDP_PORT);

	char rx_buffer[256];
    char tx_buffer[MAX_PAYLOAD_SIZE];

	while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        // Recebe o pacote UDP e armazena o endereço de origem
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &addr_len);
        
        if (len < 0) {
             ESP_LOGE(TAG, "Erro em recvfrom: %d", errno);
             continue;
        } else if (len > 0) {
            // Tratamento de string
            rx_buffer[len] = 0;

            char *end = rx_buffer + len - 1;
            while (end >= rx_buffer && (*end == '\n' || *end == '\r' || *end == ' ')) {
                *end = '\0';
                end--;
            }
            
            ESP_LOGI(TAG, "UDP RX de %s:%d: %s", 
                     inet_ntoa(source_addr.sin_addr), 
                     ntohs(source_addr.sin_port), 
                     rx_buffer);

            // 4. Processa o comando
            // Nota: Garanta que sua função process_command lida com:
            // "ping" -> Resposta JSON de pong
            // "temperatura?" ou "status" -> Resposta JSON de status
            // "ESTOP_ON" -> Aciona o estop
            process_command(rx_buffer, tx_buffer, sizeof(tx_buffer));
            
            // Envia a resposta de volta para o endereço de origem (PC)
            sendto(sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&source_addr, addr_len);
        }
	}
    ESP_LOGI(TAG, "Fechando socket UDP...");
    close(sock);
    vTaskDelete(NULL);
}

//Espera pacotes enviados pelo PC usando Protocolo TCP e devolve informações
static void tcp_server_task(void *arg) {
	int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TCP_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
	listen(listen_fd, 1);
	ESP_LOGI(TAG, "Servidor TCP na porta %d", TCP_PORT);

	while (1) {
		struct sockaddr_in6 source_addr; socklen_t addr_len = sizeof(source_addr);
		int sock = accept(listen_fd, (struct sockaddr *)&source_addr, &addr_len);
		if (sock < 0) continue;
		ESP_LOGI(TAG, "Cliente conectado");

		const char *hello = "ESP32: conectado! Digite 'status', 'estop_on', 'estop_reset' ou 'set_rpm 150.0'\n";
		send(sock, hello, strlen(hello), 0);

		char rx[256];
		while (1) {
			int len = recv(sock, rx, sizeof(rx)-1, 0);
			if (len <= 0) { ESP_LOGI(TAG, "Cliente saiu"); break; }
			rx[len] = 0;
			ESP_LOGI(TAG, "RX: %s", rx);

			char tx[MAX_PAYLOAD_SIZE];
			process_command(rx, tx, sizeof(tx));
			send(sock, tx, strlen(tx), 0);
		}
		shutdown(sock, 0);
		close(sock);
	}
}

// ===== Task: Encoders =====
static void task_enc_sense(void *arg)
{
	TickType_t next = xTaskGetTickCount();
	TickType_t T = pdMS_TO_TICKS(ENC_T_MS);
	if (T == 0) T = 1;

	for (;;) {
		if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
			stats.enc_samples++;
			xSemaphoreGive(mutexStats);
		}

		if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
			// Simula controle da velocidade da esteira
			float err = g_belt.set_rpm - g_belt.rpm;
			g_belt.rpm += 0.05f * err;
			g_belt.pos_mm += (g_belt.rpm / 60.0f) * ((float)ENC_T_MS / 1000.0f) * 100.0f;
			xSemaphoreGive(mutexBeltState);
		}

		int64_t t1 = esp_timer_get_time();
		cpu_tight_loop_us(700); // simula carga determinística
		int64_t t2 = esp_timer_get_time();
		
		if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
			cpu_time.enc_time_us += (t2 - t1);
			xSemaphoreGive(mutexStats);
		}

		// Notifica task de controle
		if (hCtrlNotify) {
			xTaskNotifyGive(hCtrlNotify);
		}

		// Aguarda próximo ciclo
		if (xTaskDelayUntil(&next, T) != pdPASS) {
			if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
				stats.enc_misses++;
				xSemaphoreGive(mutexStats);
			}
		}
	}
}

// ===== Task: Controle de velocidade (PI simulado + HMI) =====
static void task_spd_ctrl(void *arg)
{
	float kp = 0.4f, ki = 0.1f, integ = 0.f;

	for (;;) {
		// Aguarda notificação da task ENC_SENSE
		if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0) {
			if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
				stats.ctrl_misses++;
				xSemaphoreGive(mutexStats);
			}
			continue;
		}

		if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
			stats.ctrl_runs++;
			xSemaphoreGive(mutexStats);
		}

		if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
			if (estop_ativo) integ = 0.f;

			// Controle PI simples
			float err = g_belt.set_rpm - g_belt.rpm;
			integ += err * ((float)ENC_T_MS / 1000.0f);
			float u = kp * err + ki * integ;
			g_belt.rpm += 0.05f * u;

			// Reset E-stop se necessário (A task de segurança ZEROU a RPM)
			if (estop_ativo && g_belt.set_rpm > 0.f) {
				estop_ativo = false;
			}

			xSemaphoreGive(mutexBeltState);
		}

		int64_t t1 = esp_timer_get_time();
		cpu_tight_loop_us(800);
		int64_t t2 = esp_timer_get_time();

		if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
			cpu_time.ctrl_time_us += (t2 - t1);
			xSemaphoreGive(mutexStats);
		}

		int64_t t_evt_us_hmi;

		if (xQueueReceive(qHMI, &t_evt_us_hmi, 0) == pdTRUE) {
			int64_t t_start = esp_timer_get_time();

            belt_state_t hmi_state;
            get_belt_state(&hmi_state);
			
			if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE) {
				// CÁLCULO DA LATÊNCIA HMI
				int64_t latency_us = t_start - t_evt_us_hmi;
				stats.hmi_lat_total_us += latency_us;
				stats.hmi_lat_count++;
			
				printf("HMI: rpm=%.1f set=%.1f pos=%.1fmm (t=%lld | lat=%lld us)\n",
					hmi_state.rpm, hmi_state.set_rpm, hmi_state.pos_mm, (long long)t_start, (long long)latency_us);
				
				int64_t t3 = esp_timer_get_time();
				cpu_tight_loop_us(300);
				int64_t t4 = esp_timer_get_time();
				cpu_time.ctrl_time_us += (t4 - t3);

				xSemaphoreGive(mutexStats);
			}
		} 	
	}
}

// ===== Task: SORT_ACT =====
static void task_sort_act(void *arg)
{
	sort_evt_t ev;
	for (;;) {
		if (xQueueReceive(qSort, &ev, portMAX_DELAY) == pdTRUE) {
			if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
				stats.sort_events++;
				int64_t t_start = esp_timer_get_time();

				// Latência ISR -> task
				int64_t latency_us = t_start - ev.t_evt_us;
				stats.sort_lat_total_us += latency_us;
				stats.sort_lat_count++;

				int64_t t1 = esp_timer_get_time();
				cpu_tight_loop_us(500); // simula carga
				int64_t t2 = esp_timer_get_time();
				cpu_time.sort_time_us += (t2 - t1);

				printf("SORT_ACT: evento touch pad %d @ %lld -> started @ %lld (lat=%lld us)\n",
					ev.pad, (long long)ev.t_evt_us, (long long)t_start, (long long)latency_us);
				
				xSemaphoreGive(mutexStats);
			}
		}
	}
}

// ===== Task: SAFETY (E-stop) =====
static void task_safety(void *arg)
{
	safety_evt_t ev;
	for (;;) {
		if (xQueueReceive(qSafety, &ev, portMAX_DELAY) == pdTRUE) {
			if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
				stats.safety_events++;
				int64_t t_start = esp_timer_get_time();

				// Latência ISR -> task
				int64_t latency_us = t_start - ev.t_evt_us;
				stats.safety_lat_total_us += latency_us;
				stats.safety_lat_count++;

				int64_t t1 = esp_timer_get_time();
				cpu_tight_loop_us(800);
				int64_t t2 = esp_timer_get_time();
				cpu_time.safety_time_us += (t2 - t1);

				printf("SAFETY: E-STOP acionado @ %lld -> motor parado (lat=%lld us)\n",
					(long long)t_start, (long long)latency_us);
				
				xSemaphoreGive(mutexStats);
			}

			if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
				// Ação de emergência
				g_belt.rpm = 0.f;
				// Configura o setpoint para zero para que o controlador pare imediatamente
                g_belt.set_rpm = 0.f; 
				estop_ativo = true;
				xSemaphoreGive(mutexBeltState);
			}
		}
	}
}

// ===== Task: REPORT =====
static void task_report(void *arg)
{
	TickType_t next = xTaskGetTickCount();
	const float REPORT_PERIOD_MS = 60000;
	const float REPORT_PERIOD_US = REPORT_PERIOD_MS * 1000.0f;
	const TickType_t T = pdMS_TO_TICKS(REPORT_PERIOD_MS); // 60 segundos

	for (;;) {
		vTaskDelayUntil(&next, T);

		int64_t t_now = esp_timer_get_time();
		
		// Variáveis de escopo local para os resultados do ciclo
        uint32_t enc_samples_now = 0, enc_misses_now = 0, ctrl_runs_now = 0, ctrl_misses_now = 0;
        uint32_t sort_events_now = 0, sort_misses_now = 0, safety_events_now = 0, safety_misses_now = 0;
        float enc_miss_pct = 0.0f, ctrl_miss_pct = 0.0f, sort_miss_pct = 0.0f, safety_miss_pct = 0.0f;
        int64_t avg_sort_lat = 0LL, avg_safety_lat = 0LL, avg_hmi_lat = 0LL;
        int64_t total_cpu_time = 0LL;
        float cpu_usage = 0.0f;

		if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
	
			// 1. TRATAMENTO ATÔMICO do contador concorrente com a ISR
			uint32_t current_sort_misses;
			
			// variável de estado do Mutex/Seção Crítica
			portMUX_TYPE my_spinlock = portMUX_INITIALIZER_UNLOCKED;

			// Protege o acesso a stats.sort_misses da ISR
			portENTER_CRITICAL(&my_spinlock); 
			current_sort_misses = stats.sort_misses;
			portEXIT_CRITICAL(&my_spinlock);

			// Cálculo da diferença e atualização de last_stats (COM O VALOR PROTEGIDO)
			sort_misses_now = current_sort_misses - last_stats.sort_misses;
			last_stats.sort_misses = current_sort_misses;
			// FIM DO TRATAMENTO ATÔMICO

			// Calcula diferenças dos demais campos (protegidos pelo mutexStats)
			enc_samples_now = stats.enc_samples - last_stats.enc_samples;
			enc_misses_now 	= stats.enc_misses - last_stats.enc_misses;
			ctrl_runs_now 	= stats.ctrl_runs - last_stats.ctrl_runs;
			ctrl_misses_now = stats.ctrl_misses - last_stats.ctrl_misses;
			sort_events_now = stats.sort_events - last_stats.sort_events;
			// sort_misses_now já foi calculado
			safety_events_now = stats.safety_events - last_stats.safety_events;
			safety_misses_now = stats.safety_misses - last_stats.safety_misses;

			// Atualiza last_stats (APENAS os campos protegidos pelo mutex)
			last_stats.enc_samples = stats.enc_samples;
			last_stats.enc_misses 	= stats.enc_misses;
			last_stats.ctrl_runs 	= stats.ctrl_runs;
			last_stats.ctrl_misses = stats.ctrl_misses;
			last_stats.sort_events = stats.sort_events;
			// last_stats.sort_misses já foi atualizado acima de forma protegida
			last_stats.safety_events = stats.safety_events;
			last_stats.safety_misses = stats.safety_misses;

			// Cálculo de porcentagem de misses
			enc_miss_pct 	= 100.0f * enc_misses_now 	/ (enc_samples_now + enc_misses_now);
			ctrl_miss_pct = 100.0f * ctrl_misses_now / (ctrl_runs_now + ctrl_misses_now);

			sort_miss_pct = 0.0f;
			if ((sort_events_now + sort_misses_now) != 0)
				sort_miss_pct = 100.0f * sort_misses_now / (sort_events_now + sort_misses_now);

			safety_miss_pct = 0.0f;
			if ((safety_events_now + safety_misses_now) != 0)
				safety_miss_pct = 100.0f * safety_misses_now / (safety_events_now + safety_misses_now);

			avg_sort_lat = stats.sort_lat_count ? stats.sort_lat_total_us / stats.sort_lat_count : 0;
			avg_safety_lat = stats.safety_lat_count ? stats.safety_lat_total_us / stats.safety_lat_count : 0;
			avg_hmi_lat = stats.hmi_lat_count ? stats.hmi_lat_total_us / stats.hmi_lat_count : 0;

			// Uso aproximado da CPU
			total_cpu_time = cpu_time.enc_time_us + 
							cpu_time.ctrl_time_us + 
							cpu_time.sort_time_us + 
							cpu_time.safety_time_us;

			cpu_usage = (float)total_cpu_time * 100.0f / REPORT_PERIOD_US;

			// Reset tempos
			cpu_time.enc_time_us = cpu_time.ctrl_time_us = cpu_time.sort_time_us = cpu_time.safety_time_us = 0;

			// Reset dos latências
			stats.sort_lat_total_us = 0;
			stats.sort_lat_count = 0;
			stats.safety_lat_total_us = 0;
			stats.safety_lat_count = 0;
			stats.hmi_lat_total_us = 0;
			stats.hmi_lat_count = 0;

			xSemaphoreGive(mutexStats);
		}

		belt_state_t current_state;
		get_belt_state(&current_state);
		float belt_rpm = current_state.rpm;
		float belt_set_rpm = current_state.set_rpm;
		float belt_pos_mm = current_state.pos_mm;
		
		// Impressão do relatório
		ESP_LOGI(TAG, "=== REPORT (%lld us) ===", (long long)t_now);
		ESP_LOGI(TAG, "ENC_SENSE: samples=%u misses=%u", enc_samples_now, enc_misses_now);
		ESP_LOGI(TAG, "SPD_CTRL : runs=%u misses=%u", ctrl_runs_now, ctrl_misses_now);
		ESP_LOGI(TAG, "HMI latency (avg) = %lld us", (long long)avg_hmi_lat);
		ESP_LOGI(TAG, "SORT_ACT : events=%u misses=%u", sort_events_now, sort_misses_now);
		ESP_LOGI(TAG, "SORT_ACT latency (avg) = %lld us", (long long)avg_sort_lat);
		ESP_LOGI(TAG, "SAFETY : events=%u misses=%u", safety_events_now, safety_misses_now);
		ESP_LOGI(TAG, "SAFETY latency (avg) = %lld us", (long long)avg_safety_lat);
			ESP_LOGI(TAG, "Belt state: rpm=%.1f set=%.1f pos=%.1fmm",
				belt_rpm, belt_set_rpm, belt_pos_mm);
		ESP_LOGI(TAG, "CPU usage (aprox) = %.1f%%", cpu_usage);
		ESP_LOGI(TAG, "Miss pct: ENC=%.2f%% CTRL=%.2f%% SORT=%.2f%% SAFETY=%.2f%%",
			enc_miss_pct, ctrl_miss_pct, sort_miss_pct, safety_miss_pct);
		ESP_LOGI(TAG, "============================");
	}
}

// ===== ISR Touch (Sem alterações, pois já usa lógica de fila/spinlock) =====
static void IRAM_ATTR touch_cb(void* arg)
{
	uint32_t pad_intr = touch_pad_get_status(); // quais pads dispararam
	touch_pad_clear_status();

	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	int64_t now = esp_timer_get_time();

	// Variável de estado da ISR
	portMUX_TYPE my_spinlock = portMUX_INITIALIZER_UNLOCKED;

	for (int pad = 0; pad < TOUCH_PAD_MAX; pad++) {
		if (pad_intr & (1 << pad)) {
			uint16_t val = 0;
			touch_pad_read_filtered(pad, &val);
			if (val < 200) {
				switch (pad) {
					case TP_OBJ: {
						// Evento de SORT_ACT
						sort_evt_t ev = {.t_evt_us = now, .pad = TP_OBJ};
						BaseType_t sent = xQueueSendFromISR(qSort, &ev, &xHigherPriorityTaskWoken);
						if (sent != pdTRUE) {
							portENTER_CRITICAL_ISR(&my_spinlock);
							stats.sort_misses++; // Contagem de miss na ISR
							portEXIT_CRITICAL_ISR(&my_spinlock);
						}
						break;
					}
					case TP_HMI: {
						xQueueSendFromISR(qHMI, &now, &xHigherPriorityTaskWoken);
						break;
					}
					case TP_ESTOP: {
						// Safety E-stop
						safety_evt_t ev = {.t_evt_us = now, .pad = TP_ESTOP};
						xQueueSendFromISR(qSafety, &ev, &xHigherPriorityTaskWoken);
						break;
					}
				}
			}
		}
	}

	if (xHigherPriorityTaskWoken == pdTRUE) {
		portYIELD_FROM_ISR(); // força troca de contexto
	}
}

// ===== Inicialização dos touch pads =====
static void init_touch_pads(void)
{
	ESP_ERROR_CHECK(touch_pad_init());
	ESP_ERROR_CHECK(touch_pad_config(TP_OBJ, 200));
	ESP_ERROR_CHECK(touch_pad_config(TP_HMI, 200));
	ESP_ERROR_CHECK(touch_pad_config(TP_ESTOP, 200));
	ESP_ERROR_CHECK(touch_pad_filter_start(10));
	ESP_ERROR_CHECK(touch_pad_isr_register(touch_cb, NULL));
	ESP_ERROR_CHECK(touch_pad_intr_enable());
}

// ===== Main =====
void app_main(void)
{
	// Criação de semáforos e filas
	qSort = xQueueCreate(100, sizeof(sort_evt_t));
	qSafety = xQueueCreate(10, sizeof(safety_evt_t));
	qHMI = xQueueCreate(1, sizeof(int64_t));
	semEStop = xSemaphoreCreateBinary();
	mutexBeltState = xSemaphoreCreateMutex();
	mutexStats = xSemaphoreCreateMutex();

	if (!semEStop || !qHMI || !qSort || !qSafety || !mutexBeltState || !mutexStats) {
		ESP_LOGE(TAG, "Falha ao criar semáforos/filas/mutexes");
		return;
	}

	ESP_ERROR_CHECK(nvs_flash_init());
	wifi_init();

	init_touch_pads();

	// Criação de tasks
	xTaskCreatePinnedToCore(task_safety, "SAFETY", STK, NULL, PRIO_ESTOP, &hSAFE, 1);
	xTaskCreatePinnedToCore(task_enc_sense, "ENC_SENSE", STK, NULL, PRIO_ENC, &hENC, 1);
	xTaskCreatePinnedToCore(task_spd_ctrl, "SPD_CTRL", STK, NULL, PRIO_CTRL, &hCTRL, 1);
	hCtrlNotify = hCTRL; // handle para notificações
	xTaskCreatePinnedToCore(task_sort_act, "SORT_ACT", STK, NULL, PRIO_SORT, &hSORT, 1);
	xTaskCreatePinnedToCore(task_report, "REPORT", STK, NULL, PRIO_REP, &hREP, 1);
	xTaskCreatePinnedToCore(task_time, "TIME_TASK", STK, NULL, PRIO_TIME, NULL, 0);
	xTaskCreatePinnedToCore(tcp_server_task, "TCP_SERVER", STK, NULL, PRIO_TCP, NULL, 0);
	//xTaskCreatePinnedToCore(udp_server_task, "UDP_TASK", STK, NULL, PRIO_UDP, NULL, 0);

	ESP_LOGI(TAG, "Tasks criadas e esteira inicializada com Touch pads");
}