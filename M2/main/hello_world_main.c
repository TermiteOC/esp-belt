#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
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
#include "driver/gpio.h"

#define TAG "ESTEIRA"

// ===== Configuração de rede =====
#define WIFI_SSID "Nome do wifi"
#define WIFI_PASS "Senha do wifi"
#define TCP_PORT 10420 // qualquer número entre 0 e 65535
#define PC_IP 	"IP do ipconfig"
#define UDP_PORT 10421 // qualquer número entre 0 e 65535
#define REPORT_PORT 10421 // Porta onde o PC Servidor de Logs (Monitor Task) estará escutando
#define MAX_PAYLOAD_SIZE 1024 // Define um tamanho máximo para a mensagem de rede

#define TP_OBJ 	TOUCH_PAD_NUM0 	// GPIO4
#define TP_HMI 	TOUCH_PAD_NUM3 	// GPIO15
#define TP_ESTOP TOUCH_PAD_NUM7 // GPIO27
#define LED_PIN GPIO_NUM_2      // LED

// ===== Configurações de prioridade e stack =====
#define ENC_T_MS 	 5 	 	// período task enc_sense (ms)
#define PRIO_ESTOP   5
#define PRIO_ENC 	 4
#define PRIO_CTRL 	 3
#define PRIO_SORT 	 2
#define PRIO_MONITOR 5
#define PRIO_TIME 	 1
#define STK 	 	 4096 	// tamanho de stack (bytes)

// ===== Handles das tasks, filas e semáforos =====
static TaskHandle_t hENC = NULL, hCTRL = NULL, hSORT = NULL, hSAFE = NULL, hMONITOR = NULL, hTIME = NULL;
static TaskHandle_t hCtrlNotify = NULL;
static QueueHandle_t qSort = NULL; 	 	// fila de eventos SORT_ACT
static QueueHandle_t qSafety = NULL; 	// fila de eventos SAFETY
static QueueHandle_t qHMI = NULL;       // Fila para Latência HMI
//static SemaphoreHandle_t semEStop = NULL; // semáforo para E-stop
static SemaphoreHandle_t mutexBeltState = NULL; // Protege g_belt e estop_ativo
static SemaphoreHandle_t mutexStats = NULL; 	 	// Protege stats e cpu_time

static volatile bool estop_ativo = false; // flag de E-stop ativo
static int64_t max_sort_lat_us = 0;
static int64_t max_safety_lat_us = 0;

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

typedef struct {
    uint32_t enc_samples, enc_misses;
    uint32_t ctrl_runs, ctrl_misses;
    uint32_t sort_events, sort_misses;
    uint32_t safety_events, safety_misses;
    int64_t sort_lat_total_us;
    uint32_t sort_lat_count;
    int64_t safety_lat_total_us; 
    uint32_t safety_lat_count;
    int64_t hmi_lat_total_us;
    uint32_t hmi_lat_count;
} report_stats_t; // TIPO NOMEADO

typedef struct {
    int64_t enc_time_us;
    int64_t ctrl_time_us;
    int64_t sort_time_us;
    int64_t safety_time_us;
    int64_t monitor_time_us;
    int64_t time_task_us;
} report_cpu_time_t; // TIPO NOMEADO

// Métricas acumuladas (CORRIGIDO: Usa report_stats_t)
static report_stats_t stats = {0};

// Contadores de tempo ocupado da CPU (microsegundos) (CORRIGIDO: Usa report_cpu_time_t)
static report_cpu_time_t cpu_time = {0}; 

static struct {
    report_stats_t stats; 
    report_cpu_time_t cpu_time; 
    int64_t last_time_us;
} last_report_data = {0};

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
		gpio_set_level(LED_PIN, 1); // Liga o LED
    } else {
        ESP_LOGE(TAG, "Falha ao enviar evento de E-STOP para a fila!");
    }
}

void reset_estop(void) {
	// A função set_belt_setpoint já protege o acesso a g_belt.set_rpm
	set_belt_setpoint(120.f); 
	ESP_LOGI(TAG, "Comando E-STOP resetado (via cliente).");
	gpio_set_level(LED_PIN, 0); // Desliga o LED
    // A flag 'estop_ativo' é resetada pela task_spd_ctrl quando vê que o setpoint é > 0.
}

static void process_command(const char *command_rx, char *response_tx, size_t max_len) {
    float new_rpm = 0.0f;

    // Remove espaços em branco do início/fim que podem vir da rede
    char clean_command[256];
    strncpy(clean_command, command_rx, sizeof(clean_command) - 1);
    clean_command[sizeof(clean_command) - 1] = '\0'; // Garante terminação
    // Remove trailing whitespace/newline
    char *end = clean_command + strlen(clean_command) - 1;
    while(end >= clean_command && (*end == '\n' || *end == '\r' || *end == ' ')) {
        *end = '\0';
        end--;
    }

    if (strcasecmp(clean_command, "status") == 0 || strcasecmp(clean_command, "temperatura?") == 0) { // Ignora case
        belt_state_t state;
        get_belt_state(&state);
        snprintf(response_tx, max_len,
                 "{\"ok\":true,\"rpm\":%.1f,\"set_rpm\":%.1f,\"pos\":%.1f,\"estop_ativo\":%s}\n",
                 state.rpm, state.set_rpm, state.pos_mm, estop_ativo ? "true" : "false");
    } else if (strcasecmp(clean_command, "estop_on") == 0) { // Ignora case
        activate_estop();
        snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"estop_on\"}\n");
    } else if (strcasecmp(clean_command, "estop_reset") == 0) { // Ignora case
        reset_estop();
        snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"estop_reset\"}\n");
    } else if (sscanf(clean_command, "set_rpm %f", &new_rpm) == 1) {
         set_belt_setpoint(new_rpm);
         snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"set_rpm\",\"value\":%.1f}\n", new_rpm);
    } else if (strcasecmp(clean_command, "sort_act_on") == 0) {
        sort_evt_t ev = {.t_evt_us = esp_timer_get_time(), .pad = -1}; // Pad -1 indica ativação via rede
        if (xQueueSend(qSort, &ev, 0) == pdTRUE) {
             snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"sort_act_on\"}\n");
        } else {
             snprintf(response_tx, max_len, "{\"ok\":false,\"error\":\"Fila SORT_ACT cheia\"}\n");
        }
    } else if (strcasecmp(clean_command, "ping") == 0) {
        int64_t now = esp_timer_get_time();
        snprintf(response_tx, max_len,
            "{\"ok\":true,\"cmd\":\"pong\",\"timestamp_us\":%lld}\n", now);
    } else {
        snprintf(response_tx, max_len, "{\"ok\":false,\"error\":\"Comando desconhecido: '%s'\"}\n", clean_command);
    }
}

//Configura WiFi da ESP32
static void wifi_init(void) {
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
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

// --- task_time (ATUALIZADA) --- [cite: 74, 117, 118]
static void task_time(void *arg)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    uint32_t cycles_since_sync = 0; // Contador de ciclos 
    const uint32_t sync_warn_threshold = 30; // Avisa se ficar 30s sem sync

    // Configuração do Fuso Horário
    setenv("TZ", "GMT+3", 1);
    tzset();

    ESP_LOGI(TAG, "Iniciando SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_set_sync_interval(20000); 
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "pool.ntp.br");
    esp_sntp_setservername(2, "a.ntp.br");
    esp_sntp_init();

    // Espera inicial pela sincronização (pode levar algum tempo)
    ESP_LOGI(TAG, "Aguardando sincronizacao SNTP inicial...");
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[64];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Hora inicial sincronizada: %s", buf);

    TickType_t next_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(1000); // Período de 1 segundo 

    while (1) {
        // Medição de tempo de execução (C)
        int64_t t_start = esp_timer_get_time();

        cycles_since_sync++;

		// Verifica se a sincronização foi completada
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
             cycles_since_sync = 0; 
        }

		// Log da hora atual 
        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);

		// Alerta se o tempo não for sincronizado após o limiar
        if (cycles_since_sync > sync_warn_threshold) {
             ESP_LOGE(TAG, "ALERTA: SNTP sem sincronizar por %u segundos! Status: %d. Hora: %s", 
                      cycles_since_sync, sntp_get_sync_status(), buf);
        } else {
             ESP_LOGI(TAG, "Hora de Referencia: %s", buf);
        }

        // cpu_tight_loop_us(50);

        int64_t t_end = esp_timer_get_time();
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE) {
            cpu_time.time_task_us += (t_end - t_start);
            xSemaphoreGive(mutexStats);
        }

        vTaskDelayUntil(&next_wake_time, period_ticks);
    }
}

// --- MONITOR_TASK (Refatorada: Servidor de Controle + Cliente de Relatório) ---
static void monitor_task(void *arg)
{
    // =========================================================================
    // 1. SELEÇÃO DE PROTOCOLO DE COMANDO (RECEBIMENTO)
    // Descomente APENAS UMA das linhas abaixo para escolher o protocolo de controle
    #define USE_UDP 
    // #define USE_TCP 
    // =========================================================================

    // --- 1. CONFIGURAÇÃO DO SOCKET DE CONTROLE (RECEBIMENTO DE COMANDOS) ---
    int control_sock = -1;
    int client_sock = -1; // Usado apenas no modo TCP
	int report_sock = -1;
    char rx_buffer[256];
    char tx_buffer[MAX_PAYLOAD_SIZE];
    
#if defined(USE_UDP)
    // Configuração UDP (Servidor)
    control_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (control_sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket UDP de Controle.");
        goto cleanup_control;
    }

    struct sockaddr_in udp_addr = {0};
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons(UDP_PORT);
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY); 

    if (bind(control_sock, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
        ESP_LOGE(TAG, "Falha ao fazer bind do socket UDP.");
        goto cleanup_control;
    }
    
    // Configura o socket para ser não-bloqueante (retorna EAGAIN se não houver dados)
    int flags = fcntl(control_sock, F_GETFL, 0);
    if (fcntl(control_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        ESP_LOGE(TAG, "Falha ao configurar socket UDP como non-blocking");
        goto cleanup_control;
    }

    ESP_LOGI(TAG, "Monitor Task (Controle) - Servidor UDP ativo na porta %d", UDP_PORT);

#elif defined(USE_TCP)
    // Configuração TCP (Servidor)
    control_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (control_sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket TCP de Controle.");
        goto cleanup_control;
    }
    
    struct sockaddr_in tcp_addr = {0};
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port = htons(TCP_PORT);
    tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(control_sock, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) {
        ESP_LOGE(TAG, "Falha ao fazer bind do socket TCP.");
        goto cleanup_control;
    }
    
    listen(control_sock, 1);
    
    // Configura o socket LISTEN para ser não-bloqueante no accept
    int flags_listen = fcntl(control_sock, F_GETFL, 0);
    if (fcntl(control_sock, F_SETFL, flags_listen | O_NONBLOCK) == -1) {
        ESP_LOGE(TAG, "Falha ao configurar socket TCP listen como non-blocking");
        goto cleanup_control;
    }
    
    ESP_LOGI(TAG, "Monitor Task (Controle) - Servidor TCP ativo na porta %d", TCP_PORT);
#else
    ESP_LOGE(TAG, "ERRO: Nenhum protocolo de rede selecionado (USE_UDP ou USE_TCP).");
    vTaskDelete(NULL);
#endif

    // --- 2. CONFIGURAÇÃO DO SOCKET DE RELATÓRIO (ENVIO DE LOGS - SEMPRE UDP) ---
    report_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (report_sock < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket UDP para Relatório.");
        goto cleanup_control;
    }

    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(REPORT_PORT); 
    inet_pton(AF_INET, PC_IP, &dest_addr.sin_addr.s_addr);

    ESP_LOGI(TAG, "Monitor Task - Enviando status UDP para %s:%d a cada 500ms.", PC_IP, REPORT_PORT);

    TickType_t next_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(500); // Período de 500ms (D)
	int report_cycle_counter = 0;
    const int REPORT_PERIOD_CYCLES = 120; // 120 ciclos * 500ms = 60 segundos

	// Inicializa a variável de controle na primeira execução
    if (last_report_data.last_time_us == 0) {
        if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
            last_report_data.stats = stats;
            last_report_data.cpu_time = cpu_time;
            xSemaphoreGive(mutexStats);
        }
        last_report_data.last_time_us = esp_timer_get_time();
    }

    while(1) {
        // Medição de tempo de execução (C)
        int64_t t_start = esp_timer_get_time();
        
        // -------------------------------------------------------------------
        // B. RECEBIMENTO DE COMANDO (CONTROLE - SERVIDOR)
        // -------------------------------------------------------------------
#if defined(USE_UDP)
        // Lógica de recebimento UDP não-bloqueante
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        // Tenta receber um pacote sem bloquear o ciclo de 500ms
        int len = recvfrom(control_sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT, (struct sockaddr *)&source_addr, &addr_len);
        
        if (len > 0) {
            rx_buffer[len] = 0; // Termina a string
            ESP_LOGI(TAG, "UDP RX de %s:%d: %s", inet_ntoa(source_addr.sin_addr), ntohs(source_addr.sin_port), rx_buffer);
            
            // Processa comando e gera resposta
            process_command(rx_buffer, tx_buffer, sizeof(tx_buffer));
            
            // Envia a resposta de volta para o cliente UDP
            sendto(control_sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&source_addr, addr_len);

        } else if (len < 0 && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "Erro em recvfrom UDP: %d", errno);
        }

#elif defined(USE_TCP)
        // Lógica de recebimento TCP não-bloqueante e reativa
        
        if (client_sock < 0) {
            // Tenta aceitar uma nova conexão (non-blocking)
            struct sockaddr_in6 source_addr; socklen_t addr_len = sizeof(source_addr);
            client_sock = accept(control_sock, (struct sockaddr *)&source_addr, &addr_len);
            
            if (client_sock >= 0) {
                ESP_LOGI(TAG, "Cliente TCP conectado.");
                // Configura o socket do cliente para ser não-bloqueante (para não travar o loop de 500ms)
                int flags_client = fcntl(client_sock, F_GETFL, 0);
                fcntl(client_sock, F_SETFL, flags_client | O_NONBLOCK);

                const char *hello = "ESP32: conectado! Digite 'status', 'estop_on', 'estop_reset' ou 'set_rpm 150.0'\n";
                send(client_sock, hello, strlen(hello), 0);
            } else if (errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "Erro em accept TCP: %d", errno);
            }
        }
        
        if (client_sock >= 0) {
            // Tenta receber dados do cliente conectado (non-blocking)
            int len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT);
            
            if (len > 0) {
                rx_buffer[len] = 0;
                ESP_LOGI(TAG, "TCP RX: %s", rx_buffer);
                
                // Processa comando e gera resposta
                process_command(rx_buffer, tx_buffer, sizeof(tx_buffer));
                send(client_sock, tx_buffer, strlen(tx_buffer), 0);
            
            } else if (len == 0 || (len < 0 && errno != EWOULDBLOCK)) {
                // Cliente desconectou (len == 0) ou erro real
                ESP_LOGI(TAG, "Cliente TCP saiu/erro: %d", errno);
                shutdown(client_sock, 0);
                close(client_sock);
                client_sock = -1; // Pronto para aceitar um novo cliente
            }
        }
#endif

		report_cycle_counter++;

		if (report_cycle_counter >= REPORT_PERIOD_CYCLES) {
            report_cycle_counter = 0;
            
            // Variáveis de coleta de dados no período
            uint32_t enc_samples_delta, enc_misses_delta, ctrl_runs_delta, ctrl_misses_delta;
            uint32_t sort_events_delta, sort_misses_delta, safety_events_delta, safety_misses_delta;
            int64_t sort_lat_total_delta, safety_lat_total_delta, hmi_lat_total_delta;
            uint32_t sort_lat_count_delta, safety_lat_count_delta, hmi_lat_count_delta;
            int64_t cpu_time_total_delta = 0;
            int64_t total_period_us;
            
            belt_state_t current_belt;
            get_belt_state(&current_belt);
            
            int64_t t_now = esp_timer_get_time();

            if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(50)) == pdTRUE) {
                // Cálculo de Deltas
                enc_samples_delta = stats.enc_samples - last_report_data.stats.enc_samples;
                enc_misses_delta = stats.enc_misses - last_report_data.stats.enc_misses;
                ctrl_runs_delta = stats.ctrl_runs - last_report_data.stats.ctrl_runs;
                ctrl_misses_delta = stats.ctrl_misses - last_report_data.stats.ctrl_misses;
                sort_events_delta = stats.sort_events - last_report_data.stats.sort_events;
                sort_misses_delta = stats.sort_misses - last_report_data.stats.sort_misses;
                safety_events_delta = stats.safety_events - last_report_data.stats.safety_events;
                safety_misses_delta = stats.safety_misses - last_report_data.stats.safety_misses;

                sort_lat_total_delta = stats.sort_lat_total_us - last_report_data.stats.sort_lat_total_us;
                sort_lat_count_delta = stats.sort_lat_count - last_report_data.stats.sort_lat_count;
                safety_lat_total_delta = stats.safety_lat_total_us - last_report_data.stats.safety_lat_total_us;
                safety_lat_count_delta = stats.safety_lat_count - last_report_data.stats.safety_lat_count;
                hmi_lat_total_delta = stats.hmi_lat_total_us - last_report_data.stats.hmi_lat_total_us;
                hmi_lat_count_delta = stats.hmi_lat_count - last_report_data.stats.hmi_lat_count;

                // Cálculo de Tempo de CPU Total
                cpu_time_total_delta += cpu_time.enc_time_us - last_report_data.cpu_time.enc_time_us;
                cpu_time_total_delta += cpu_time.ctrl_time_us - last_report_data.cpu_time.ctrl_time_us;
                cpu_time_total_delta += cpu_time.sort_time_us - last_report_data.cpu_time.sort_time_us;
                cpu_time_total_delta += cpu_time.safety_time_us - last_report_data.cpu_time.safety_time_us;
                cpu_time_total_delta += cpu_time.monitor_time_us - last_report_data.cpu_time.monitor_time_us;
                cpu_time_total_delta += cpu_time.time_task_us - last_report_data.cpu_time.time_task_us;

                // ATUALIZAÇÃO para o próximo ciclo
                last_report_data.stats = stats;
                last_report_data.cpu_time = cpu_time;
                xSemaphoreGive(mutexStats);
            } else {
                ESP_LOGE(TAG, "Falha ao obter mutexStats para Relatório!");
                goto end_report_cycle; // Pula este ciclo de relatório se falhar o mutex
            }

            // Cálculo do tempo total do período
            total_period_us = t_now - last_report_data.last_time_us;
            last_report_data.last_time_us = t_now;

            // CÁLCULO DE MÉDIAS E PERCENTUAIS
            int64_t avg_sort_lat = sort_lat_count_delta > 0 ? sort_lat_total_delta / sort_lat_count_delta : 0;
            int64_t avg_safety_lat = safety_lat_count_delta > 0 ? safety_lat_total_delta / safety_lat_count_delta : 0;
            int64_t avg_hmi_lat = hmi_lat_count_delta > 0 ? hmi_lat_total_delta / hmi_lat_count_delta : 0;

            float cpu_usage = (float)cpu_time_total_delta * 100.0f / (float)total_period_us;
            
            float enc_miss_pct = enc_samples_delta > 0 ? (float)enc_misses_delta * 100.0f / (float)enc_samples_delta : 0.0f;
            float ctrl_miss_pct = ctrl_runs_delta > 0 ? (float)ctrl_misses_delta * 100.0f / (float)ctrl_runs_delta : 0.0f;
            float sort_miss_pct = sort_events_delta > 0 ? (float)sort_misses_delta * 100.0f / (float)sort_events_delta : 0.0f;
            float safety_miss_pct = safety_events_delta > 0 ? (float)safety_misses_delta * 100.0f / (float)safety_events_delta : 0.0f;

            // FORMATA O JSON COMPLETO
                snprintf(tx_buffer, sizeof(tx_buffer),
                "=== REPORT (%lld us) ===\n"
                "ENC_SENSE: samples=%lu misses=%lu\n"
                "SPD_CTRL : runs=%lu misses=%lu\n"
                "HMI latency (avg) = %lld us\n"
                "SORT_ACT : events=%lu misses=%lu\n"
                "SORT_ACT latency (avg) = %lld us | max = %lld us\n"
                "SAFETY   : events=%lu misses=%lu\n"
                "SAFETY latency (avg) = %lld us | max = %lld us\n"
                "Belt state: rpm=%.1f set=%.1f pos=%.1fmm\n"
                "CPU usage (aprox) = %.1f%%\n"
                "Miss pct: ENC=%.2f%% CTRL=%.2f%% SORT=%.2f%% SAFETY=%.2f%%\n"
                "============================\n",
                (long long)t_now,
                enc_samples_delta, enc_misses_delta,
                ctrl_runs_delta, ctrl_misses_delta,
                (long long)avg_hmi_lat,
                sort_events_delta, sort_misses_delta, (long long)avg_sort_lat, (long long)max_sort_lat_us,
                safety_events_delta, safety_misses_delta, (long long)avg_safety_lat, (long long)max_safety_lat_us,
                current_belt.rpm, current_belt.set_rpm, current_belt.pos_mm,
                cpu_usage, enc_miss_pct, ctrl_miss_pct, sort_miss_pct, safety_miss_pct
            );

            // ENVIAR O RELATÓRIO UDP
            sendto(report_sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            ESP_LOGI(TAG, "Relatorio Estatistico Enviado.");

            max_sort_lat_us = 0;
            max_safety_lat_us = 0;
        }

		end_report_cycle:;

        int64_t t_end = esp_timer_get_time();
        // Atualiza tempo de CPU (protegido por mutex)
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE) {
            cpu_time.monitor_time_us += (t_end - t_start);
            xSemaphoreGive(mutexStats);
        }

        // Aguarda próximo ciclo periodicamente 
        vTaskDelayUntil(&next_wake_time, period_ticks);
    }

cleanup_control:
    // Garante que ambos os sockets sejam fechados, se criados
    if (control_sock >= 0) close(control_sock);
    if (report_sock >= 0) close(report_sock);
    if (client_sock >= 0) close(client_sock);

    vTaskDelete(NULL);
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
                if (latency_us > max_sort_lat_us) {
                    max_sort_lat_us = latency_us;
                }

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
                if (latency_us > max_safety_lat_us) {
                    max_safety_lat_us = latency_us;
                }

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
	//semEStop = xSemaphoreCreateBinary();
	mutexBeltState = xSemaphoreCreateMutex();
	mutexStats = xSemaphoreCreateMutex();

	if (!qHMI || !qSort || !qSafety || !mutexBeltState || !mutexStats) {
		ESP_LOGE(TAG, "Falha ao criar semáforos/filas/mutexes");
		return;
	}

	gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0); // LED começa desligado

	ESP_ERROR_CHECK(nvs_flash_init());
	wifi_init();
	init_touch_pads();

	// Criação de tasks
	xTaskCreatePinnedToCore(task_safety, "SAFETY", STK, NULL, PRIO_ESTOP, &hSAFE, 1);
	xTaskCreatePinnedToCore(task_enc_sense, "ENC_SENSE", STK, NULL, PRIO_ENC, &hENC, 1);
	xTaskCreatePinnedToCore(task_spd_ctrl, "SPD_CTRL", STK, NULL, PRIO_CTRL, &hCTRL, 1);
	hCtrlNotify = hCTRL; // handle para notificações
	xTaskCreatePinnedToCore(task_sort_act, "SORT_ACT", STK, NULL, PRIO_SORT, &hSORT, 1);
	xTaskCreatePinnedToCore(task_time, "TIME_TASK", STK, NULL, PRIO_TIME, &hTIME, 0);
	xTaskCreatePinnedToCore(monitor_task, "MONITOR", STK, NULL, PRIO_MONITOR, &hMONITOR, 0);

	ESP_LOGI(TAG, "Tasks criadas e esteira inicializada com Touch pads");
}
