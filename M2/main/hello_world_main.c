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
#define WIFI_SSID "Acqua Medeiros 2.4G"
#define WIFI_PASS "28041983"

// =========================================================================
//  SELEÇÃO DE PROTOCOLO DE CONTROLE
//  Define qual protocolo a 'monitor_task' usará para RECEBER comandos.
//  1 = UDP (Servidor UDP na porta UDP_PORT)
//  0 = TCP (Servidor TCP na porta TCP_PORT)
#define CONTROL_PROTOCOL_IS_UDP 1
// =========================================================================

#define TCP_PORT 10420 // Porta para o servidor de controle TCP
#define PC_IP 	"192.168.18.10" // IP do PC que recebe os relatórios de telemetria
#define UDP_PORT 10421 // Porta para o servidor de controle UDP
#define REPORT_PORT 10421 // Porta no PC onde os relatórios de telemetria serão enviados
#define MAX_PAYLOAD_SIZE 1024 

// --- Configuração da análise (m,k)-firm ---
#define MK_K 10              // k = tamanho da janela de eventos
#define MK_M 9               // m = mínimo de eventos que devem cumprir a deadline

// --- Mapeamento de Hardware ---
#define TP_OBJ 	TOUCH_PAD_NUM0 	// GPIO4
#define TP_HMI 	TOUCH_PAD_NUM3 	// GPIO15
#define TP_ESTOP TOUCH_PAD_NUM7 // GPIO27
#define LED_PIN GPIO_NUM_2      // LED

// ===== Configurações de prioridade das Tarefas RTOS =====
// Valores mais altos têm prioridade maior.
#define PRIO_ESTOP   5
#define PRIO_ENC 	 4
#define PRIO_CTRL 	 3
#define PRIO_SORT 	 3 
#define PRIO_MONITOR 5 // Prioridade alta para rede (comandos e telemetria)
#define PRIO_TIME 	 1 // Prioridade baixa para sincronia de relógio
#define STK 	 	 4096 	// tamanho de stack (bytes)

// ===== Handles das tasks, filas e semáforos =====
static TaskHandle_t hENC = NULL, hCTRL = NULL, hSORT = NULL, hSAFE = NULL, hMONITOR = NULL, hTIME = NULL;
static TaskHandle_t hCtrlNotify = NULL; // Usado para notificar a task de controle
static QueueHandle_t qSort = NULL; 	 	// Fila de eventos para a esteira (task_sort_act)
static QueueHandle_t qSafety = NULL; 	// Fila de eventos de E-Stop (task_safety)
static QueueHandle_t qHMI = NULL;       // Fila para eventos da IHM (Touch HMI)
static SemaphoreHandle_t mutexBeltState = NULL; // Protege o acesso à estrutura 'g_belt'
static SemaphoreHandle_t mutexStats = NULL; 	// Protege o acesso às estruturas 'stats' e 'cpu_time'

static volatile bool estop_ativo = false; // flag global de E-stop
static int64_t max_sort_lat_us = 0;
static int64_t max_safety_lat_us = 0;
static uint32_t cycles_since_sync_global = 0;

// ===== Estrutura de Estado da Esteira =====
// Armazena o estado atual da esteira.
typedef struct {
    float rpm; 	 	// velocidade atual (rpm)
    float pos_mm; 	// posição em mm
    float set_rpm; 	// setpoint de rpm
} belt_state_t;

static belt_state_t g_belt = {0.f, 0.f, 120.f}; // Estado inicial

// --- Estruturas para passagem de eventos entre ISR e Tarefas ---
typedef struct {
    int64_t t_evt_us; // timestamp de quando o evento ocorreu (na ISR)
    int pad; 	 	  // qual touch pad
} sort_evt_t;

typedef struct {
    int64_t t_evt_us;
    int pad;
} safety_evt_t;

// --- Estrutura para acumular estatísticas de desempenho ---
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
} report_stats_t;

// --- Estrutura para acumular tempo de CPU por tarefa ---
typedef struct {
    int64_t enc_time_us;
    int64_t ctrl_time_us;
    int64_t sort_time_us;
    int64_t safety_time_us;
    int64_t monitor_time_us;
    int64_t time_task_us;
} report_cpu_time_t;

// --- Estrutura para (m,k)-firm ---
// Mantém uma janela deslizante de resultados (cumpriu/falhou deadline).
typedef struct {
    uint8_t window[MK_K];
    int idx;
    int count_true; // quantas deadlines cumpridas na janela atual
} mk_firm_t;

static mk_firm_t mk_sort = { {0}, 0, 0 };
static mk_firm_t mk_safety = { {0}, 0, 0 };

static int64_t sort_lat_buf[LAT_BUFFER_SIZE];
static size_t sort_lat_buf_idx = 0;
static int64_t sort_max_lat = 0;

static int64_t safety_lat_buf[LAT_BUFFER_SIZE];
static size_t safety_lat_buf_idx = 0;
static int64_t safety_max_lat = 0;

static int64_t monitor_seq = 0; // Número sequencial dos relatórios de telemetria
static report_stats_t stats = {0}; // Variável global de estatísticas
static report_cpu_time_t cpu_time = {0}; // Variável global de tempo de CPU

// Estrutura para calcular deltas no relatório de telemetria
static struct {
    report_stats_t stats; 
    report_cpu_time_t cpu_time; 
    int64_t last_time_us;
} last_report_data = {0};

// Protótipos de função
void activate_estop(void);
void set_belt_setpoint(float rpm);
void reset_estop(void);
void get_belt_state(belt_state_t *state);
static void process_command(const char *command_rx, char *response_tx, size_t max_len);

// --- cpu_tight_loop_us ---
// Função: Simula uma carga de processamento (CPU-bound) por 'us' microssegundos.
// Usada para simular o trabalho real das tarefas de controle.
static inline void cpu_tight_loop_us(uint32_t us)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < (int64_t)us) {
        asm volatile("nop");
    }
}

// --- set_belt_setpoint ---
// Função: Define o setpoint (velocidade desejada) da esteira.
// Design: Usa um Mutex (mutexBeltState) para proteger a variável 
//         global 'g_belt' contra acesso concorrente por múltiplas tarefas.
void set_belt_setpoint(float rpm) {
    if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
        g_belt.set_rpm = rpm; 
        xSemaphoreGive(mutexBeltState);
    }
}

// --- get_belt_state ---
// Função: Obtém o estado atual da esteira (rpm, pos, set_rpm).
// Design: Também usa o Mutex 'mutexBeltState' para garantir uma 
//         leitura "atômica" (consistente) da estrutura de dados.
void get_belt_state(belt_state_t *state) {
    if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
        memcpy(state, &g_belt, sizeof(belt_state_t));
        xSemaphoreGive(mutexBeltState);
    } else {
        memset(state, 0, sizeof(belt_state_t));
    }
}

// --- activate_estop ---
// Função: Aciona a parada de emergência (chamada via comando de rede).
// Design: Não para a esteira diretamente. Ela envia um evento 
//         para a fila 'qSafety', que será processado pela 
//         'task_safety' (que tem a prioridade mais alta).
void activate_estop(void) {
    safety_evt_t ev = {.t_evt_us = esp_timer_get_time(), .pad = TP_ESTOP};
    if (xQueueSend(qSafety, &ev, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Comando E-STOP acionado (via cliente).");
        gpio_set_level(LED_PIN, 1); // Liga o LED
    } else {
        ESP_LOGE(TAG, "Falha ao enviar evento de E-STOP para a fila!");
    }
}

// --- reset_estop ---
// Função: Libera a parada de emergência (chamada via comando de rede).
// Design: Apenas define o setpoint de volta para um valor positivo.
//         A lógica real de reset (zerar 'estop_ativo') é 
//         tratada pela 'task_spd_ctrl'.
void reset_estop(void) {
    set_belt_setpoint(120.f); 
    ESP_LOGI(TAG, "Comando E-STOP resetado (via cliente).");
    gpio_set_level(LED_PIN, 0); // Desliga o LED
}

// --- process_command ---
// Função: O "cérebro" do servidor de rede.
// Design: Recebe uma string de comando (ex: "status", "set_rpm 150"),
//         analisa (parse) a string e chama as funções de controle 
//         apropriadas (get_belt_state, set_belt_setpoint, etc.).
//         Formata uma resposta em JSON para enviar de volta ao cliente.
static void process_command(const char *command_rx, char *response_tx, size_t max_len) {
    float new_rpm = 0.0f;
    char clean_command[256];
    
    // Limpa a string de entrada
    strncpy(clean_command, command_rx, sizeof(clean_command) - 1);
    clean_command[sizeof(clean_command) - 1] = '\0'; 
    char *end = clean_command + strlen(clean_command) - 1;
    while(end >= clean_command && (*end == '\n' || *end == '\r' || *end == ' ')) {
        *end = '\0';
        end--;
    }

    // Analisa o comando
    if (strcasecmp(clean_command, "status") == 0) {
        belt_state_t state;
        get_belt_state(&state);
        snprintf(response_tx, max_len,
                "{\"ok\":true,\"rpm\":%.1f,\"set_rpm\":%.1f,\"pos\":%.1f,\"estop_ativo\":%s}\n",
                state.rpm, state.set_rpm, state.pos_mm, estop_ativo ? "true" : "false");
    } else if (strcasecmp(clean_command, "estop_on") == 0) {
        activate_estop();
        snprintf(response_tx, max_len, "{\"ok\":true,\"cmd\":\"estop_on\"}\n");
    } else if (strcasecmp(clean_command, "estop_reset") == 0) {
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

// --- wifi_init ---
// Função: Inicializa a interface de rede e conecta o ESP32 ao Wi-Fi 
//         configurado (WIFI_SSID e WIFI_PASS).
static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // Desativa power save para menor latência
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

// Callback para logar quando o tempo é sincronizado
static void time_sync_notification_cb(struct timeval *tv){
    ESP_LOGI(TAG, "Tempo sincronizado via SNTP.");
}

// --- task_time ---
// Função: Tarefa de baixa prioridade responsável por manter o relógio 
//         do sistema sincronizado com a internet (NTP/SNTP).
// Design: Roda a cada 1 segundo, loga a hora atual e verifica o 
//         status da sincronização SNTP.
static void task_time(void *arg)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    uint32_t cycles_since_sync = 0; 
    const uint32_t sync_warn_threshold = 30;

    setenv("TZ", "GMT+3", 1); // Configura fuso horário
    tzset();

    ESP_LOGI(TAG, "Iniciando SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_set_sync_interval(20000); 
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_setservername(0, "time.google.com");
    esp_sntp_setservername(1, "pool.ntp.br");
    esp_sntp_init();

    // Espera inicial pela sincronização
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    TickType_t next_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(1000); // Período de 1 segundo 

    while (1) {
        int64_t t_start = esp_timer_get_time();
        cycles_since_sync++;

        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE) {
            cycles_since_sync_global = cycles_since_sync;
            xSemaphoreGive(mutexStats);
        }

        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            cycles_since_sync = 0; 
        }

        // Log da hora atual 
        time(&now);
        localtime_r(&now, &timeinfo);
        char buf[64];
        strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &timeinfo);

        if (cycles_since_sync > sync_warn_threshold) {
            ESP_LOGE(TAG, "ALERTA: SNTP sem sincronizar por %u segundos! Hora: %s", 
                    cycles_since_sync, buf);
        } else {
            ESP_LOGI(TAG, "Hora de Referencia: %s", buf);
        }

        int64_t t_end = esp_timer_get_time();
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE) {
            cpu_time.time_task_us += (t_end - t_start);
            xSemaphoreGive(mutexStats);
        }

        vTaskDelayUntil(&next_wake_time, period_ticks);
    }
}

// --- monitor_task ---
// Função: Tarefa principal de comunicação de rede (Servidor e Cliente).
// Design: Possui duas funções principais e roda no Core 0:
//         1. (Servidor de Comando): Ouve comandos (via TCP ou UDP, 
//            definido por 'CONTROL_PROTOCOL_IS_UDP') de forma 
//            NÃO-BLOQUEANTE.
//         2. (Cliente de Telemetria): Envia relatórios de diagnóstico 
//            periódicos (sempre via UDP) para o PC_IP.
static void monitor_task(void *arg)
{
    // --- 1. CONFIGURAÇÃO DO SOCKET DE CONTROLE (RECEBIMENTO DE COMANDOS) ---
    int control_sock = -1;
    int client_sock = -1; // Usado apenas no modo TCP
    int report_sock = -1; // Socket para enviar telemetria
    char rx_buffer[256];
    char tx_buffer[MAX_PAYLOAD_SIZE];
    
#if CONTROL_PROTOCOL_IS_UDP == 1
    // Configuração UDP (Servidor)
    control_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in udp_addr = {0};
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons(UDP_PORT);
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    bind(control_sock, (struct sockaddr*)&udp_addr, sizeof(udp_addr));
    
    // Configura o socket para ser não-bloqueante
    int flags = fcntl(control_sock, F_GETFL, 0);
    fcntl(control_sock, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "Monitor Task (Controle) - Servidor UDP ativo na porta %d", UDP_PORT);

#else // (CONTROL_PROTOCOL_IS_UDP == 0) -> TCP
    // Configuração TCP (Servidor)
    control_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in tcp_addr = {0};
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port = htons(TCP_PORT);
    tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(control_sock, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr));
    listen(control_sock, 1);
    
    // Configura o socket LISTEN para ser não-bloqueante no accept
    int flags_listen = fcntl(control_sock, F_GETFL, 0);
    fcntl(control_sock, F_SETFL, flags_listen | O_NONBLOCK);
    ESP_LOGI(TAG, "Monitor Task (Controle) - Servidor TCP ativo na porta %d", TCP_PORT);
#endif

    // --- 2. CONFIGURAÇÃO DO SOCKET DE RELATÓRIO (ENVIO DE LOGS - SEMPRE UDP) ---
    report_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(REPORT_PORT); 
    inet_pton(AF_INET, PC_IP, &dest_addr.sin_addr.s_addr);
    ESP_LOGI(TAG, "Monitor Task - Enviando status UDP para %s:%d.", PC_IP, REPORT_PORT);

    TickType_t next_wake_time = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(500); // Período de 500ms
    int report_cycle_counter = 0;
    const int REPORT_PERIOD_CYCLES = 60; // 60 ciclos * 500ms = 30 segundos

    // Inicializa dados de controle
    if (last_report_data.last_time_us == 0) {
        if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
            last_report_data.stats = stats;
            last_report_data.cpu_time = cpu_time;
            xSemaphoreGive(mutexStats);
        }
        last_report_data.last_time_us = esp_timer_get_time();
    }

    while(1) {
        int64_t t_start = esp_timer_get_time();
        
        // --- B. RECEBIMENTO DE COMANDO (CONTROLE - SERVIDOR) ---
        // Esta seção é não-bloqueante; ela checa por dados e continua.
#if CONTROL_PROTOCOL_IS_UDP == 1
        // Lógica de recebimento UDP não-bloqueante
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        // Tenta receber um pacote (MSG_DONTWAIT)
        int len = recvfrom(control_sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT, (struct sockaddr *)&source_addr, &addr_len);
        
        if (len > 0) {
            rx_buffer[len] = 0; // Termina a string
            ESP_LOGI(TAG, "UDP RX de %s:%d: %s", inet_ntoa(source_addr.sin_addr), ntohs(source_addr.sin_port), rx_buffer);
            
            process_command(rx_buffer, tx_buffer, sizeof(tx_buffer));
            
            // Envia a resposta de volta para o cliente UDP
            sendto(control_sock, tx_buffer, strlen(tx_buffer), 0, (struct sockaddr *)&source_addr, addr_len);
        }
#else // (CONTROL_PROTOCOL_IS_UDP == 0) -> TCP
        // Lógica de recebimento TCP não-bloqueante
        
        // 1. Se não houver cliente conectado, tenta aceitar um (non-blocking)
        if (client_sock < 0) {
            struct sockaddr_in6 source_addr; socklen_t addr_len = sizeof(source_addr);
            client_sock = accept(control_sock, (struct sockaddr *)&source_addr, &addr_len);
            
            if (client_sock >= 0) {
                ESP_LOGI(TAG, "Cliente TCP conectado.");
                // Configura o socket do NOVO cliente para ser não-bloqueante
                int flags_client = fcntl(client_sock, F_GETFL, 0);
                fcntl(client_sock, F_SETFL, flags_client | O_NONBLOCK);

                const char *hello = "ESP32: conectado!\n";
                send(client_sock, hello, strlen(hello), 0);
            }
        }
        
        // 2. Se houver um cliente, tenta ler dados dele (non-blocking)
        if (client_sock >= 0) {
            int len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT);
            
            if (len > 0) {
                // Dados recebidos
                rx_buffer[len] = 0;
                ESP_LOGI(TAG, "TCP RX: %s", rx_buffer);
                process_command(rx_buffer, tx_buffer, sizeof(tx_buffer));
                send(client_sock, tx_buffer, strlen(tx_buffer), 0);
            } else if (len == 0 || (len < 0 && errno != EWOULDBLOCK)) {
                // Cliente desconectou (len == 0) ou erro
                ESP_LOGI(TAG, "Cliente TCP saiu.");
                shutdown(client_sock, 0);
                close(client_sock);
                client_sock = -1; // Pronto para aceitar um novo cliente
            }
        }
#endif

        // --- C. ENVIO DE RELATÓRIO DE TELEMETRIA (CLIENTE) ---
        report_cycle_counter++;

        // Verifica se é hora de enviar o relatório periódico
        if (report_cycle_counter >= REPORT_PERIOD_CYCLES) {
            report_cycle_counter = 0;
            
            // Variáveis para calcular os deltas do período
            uint32_t enc_samples_delta, enc_misses_delta, ctrl_runs_delta, ctrl_misses_delta;
            uint32_t sort_events_delta, sort_misses_delta, safety_events_delta, safety_misses_delta;
            int64_t sort_lat_total_delta, safety_lat_total_delta;
            uint32_t sort_lat_count_delta, safety_lat_count_delta;
            int64_t cpu_time_total_delta = 0;
            int64_t total_period_us;
            
            belt_state_t current_belt;
            get_belt_state(&current_belt);
            int64_t t_now = esp_timer_get_time();

            // Protege o acesso às estatísticas globais para calcular os deltas
            if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(50)) == pdTRUE) {
                // Cálculo de Deltas (Valor Atual - Valor Anterior)
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

                // Cálculo de Tempo de CPU Total no período
                cpu_time_total_delta += cpu_time.enc_time_us - last_report_data.cpu_time.enc_time_us;
                cpu_time_total_delta += cpu_time.ctrl_time_us - last_report_data.cpu_time.ctrl_time_us;
                cpu_time_total_delta += cpu_time.sort_time_us - last_report_data.cpu_time.sort_time_us;
                cpu_time_total_delta += cpu_time.safety_time_us - last_report_data.cpu_time.safety_time_us;
                cpu_time_total_delta += cpu_time.monitor_time_us - last_report_data.cpu_time.monitor_time_us;
                cpu_time_total_delta += cpu_time.time_task_us - last_report_data.cpu_time.time_task_us;

                // Salva os valores atuais para o próximo ciclo
                last_report_data.stats = stats;
                last_report_data.cpu_time = cpu_time;
                xSemaphoreGive(mutexStats);
            } else {
                ESP_LOGE(TAG, "Falha ao obter mutexStats para Relatório!");
                goto end_report_cycle; // Pula este ciclo de relatório
            }

            total_period_us = t_now - last_report_data.last_time_us;
            last_report_data.last_time_us = t_now;

            // --- Cálculos de Médias e Percentuais ---
            int64_t avg_sort_lat = sort_lat_count_delta > 0 ? sort_lat_total_delta / sort_lat_count_delta : 0;
            int64_t avg_safety_lat = safety_lat_count_delta > 0 ? safety_lat_total_delta / safety_lat_count_delta : 0;
            float cpu_usage = (float)cpu_time_total_delta * 100.0f / (float)total_period_us;
            
            monitor_seq++;

            // Obtém o tempo atual (SNTP) para o relatório
            struct timeval tv;
            gettimeofday(&tv, NULL);
            int pkt_size = 0;

            // Etapa 1: Formata o JSON (com pkt_size = 0) para descobrir o tamanho
            pkt_size = snprintf(tx_buffer, sizeof(tx_buffer),
                "{\n"
                "  \"type\":\"monitor_report\",\n"
                "  \"seq\":%lld,\n"
                "  \"protocol\":\"UDP\",\n"
                "  \"pkt_size\":%d,\n" // Placeholder
                "  \"sntp_time_s\":%lld,\n"
                "  \"sntp_time_us\":%lld,\n"
                "  \"esp_time_us\":%lld,\n"
                "  \"period_us\":%lld,\n"
                "  \"enc\": { \"samples\": %lu, \"misses\": %lu },\n"
                "  \"ctrl\": { \"runs\": %lu, \"misses\": %lu },\n"
                "  \"sort\": { \"events\": %lu, \"misses\": %lu, \"avg_lat_us\": %lld, \"max_lat_us\": %lld },\n"
                "  \"safety\": { \"events\": %lu, \"misses\": %lu, \"avg_lat_us\": %lld, \"max_lat_us\": %lld },\n"
                "  \"belt\": { \"rpm\": %.1f, \"set_rpm\": %.1f, \"pos_mm\": %.1f },\n"
                "  \"cpu_usage_pct\": %.2f,\n"
                "  \"mk_sort\": { \"m\": %d, \"k\": %d, \"met_in_window\": %d },\n"
                "  \"mk_safety\": { \"m\": %d, \"k\": %d, \"met_in_window\": %d }\n"
                "}\n",
                (long long)monitor_seq,
                0, // O placeholder
                (long long)tv.tv_sec, (long long)tv.tv_usec, (long long)t_now, (long long)total_period_us,
                enc_samples_delta, enc_misses_delta,
                ctrl_runs_delta, ctrl_misses_delta,
                sort_events_delta, sort_misses_delta, (long long)avg_sort_lat, (long long)sort_max_lat,
                safety_events_delta, safety_misses_delta, (long long)avg_safety_lat, (long long)safety_max_lat,
                current_belt.rpm, current_belt.set_rpm, current_belt.pos_mm,
                cpu_usage,
                MK_M, MK_K, mk_sort.count_true,
                MK_M, MK_K, mk_safety.count_true
            );

            // Etapa 2: Re-formata o JSON, agora com o tamanho (pkt_size) correto
            pkt_size = snprintf(tx_buffer, sizeof(tx_buffer),
                "{\n"
                "  \"type\":\"monitor_report\",\n"
                "  \"seq\":%lld,\n"
                "  \"protocol\":\"UDP\",\n"
                "  \"pkt_size\":%d,\n" // Valor correto
                "  \"sntp_time_s\":%lld,\n"
                "  \"sntp_time_us\":%lld,\n"
                "  \"esp_time_us\":%lld,\n"
                "  \"period_us\":%lld,\n"
                "  \"enc\": { \"samples\": %lu, \"misses\": %lu },\n"
                "  \"ctrl\": { \"runs\": %lu, \"misses\": %lu },\n"
                "  \"sort\": { \"events\": %lu, \"misses\": %lu, \"avg_lat_us\": %lld, \"max_lat_us\": %lld },\n"
                "  \"safety\": { \"events\": %lu, \"misses\": %lu, \"avg_lat_us\": %lld, \"max_lat_us\": %lld },\n"
                "  \"belt\": { \"rpm\": %.1f, \"set_rpm\": %.1f, \"pos_mm\": %.1f },\n"
                "  \"cpu_usage_pct\": %.2f,\n"
                "  \"mk_sort\": { \"m\": %d, \"k\": %d, \"met_in_window\": %d },\n"
                "  \"mk_safety\": { \"m\": %d, \"k\": %d, \"met_in_window\": %d }\n"
                "}\n",
                (long long)monitor_seq,
                pkt_size, // Insere o tamanho calculado
                (long long)tv.tv_sec, (long long)tv.tv_usec, (long long)t_now, (long long)total_period_us,
                enc_samples_delta, enc_misses_delta,
                ctrl_runs_delta, ctrl_misses_delta,
                sort_events_delta, sort_misses_delta, (long long)avg_sort_lat, (long long)sort_max_lat,
                safety_events_delta, safety_misses_delta, (long long)avg_safety_lat, (long long)safety_max_lat,
                current_belt.rpm, current_belt.set_rpm, current_belt.pos_mm,
                cpu_usage,
                MK_M, MK_K, mk_sort.count_true,
                MK_M, MK_K, mk_safety.count_true
            );
            
            // ENVIAR O RELATÓRIO UDP para o PC
            sendto(report_sock, tx_buffer, pkt_size, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            ESP_LOGI(TAG, "Relatorio Estatistico Enviado (seq=%lld size=%d).", (long long)monitor_seq, pkt_size);

            // Reseta as latências máximas para o próximo período
            max_sort_lat_us = 0;
            max_safety_lat_us = 0;
        }

        end_report_cycle:; // Ponto de saída caso o mutex falhe

        int64_t t_end = esp_timer_get_time();
        if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE) {
            cpu_time.monitor_time_us += (t_end - t_start);
            xSemaphoreGive(mutexStats);
        }

        vTaskDelayUntil(&next_wake_time, period_ticks); // Aguarda o próximo ciclo de 500ms
    }

// Rótulo para limpeza em caso de falha na inicialização dos sockets
cleanup_control:
    if (control_sock >= 0) close(control_sock);
    if (report_sock >= 0) close(report_sock);
    if (client_sock >= 0) close(client_sock);
    vTaskDelete(NULL);
}

// --- task_enc_sense ---
// Função: Simula a leitura de sensores (Encoder) da esteira.
// Design: Roda periodicamente (a cada ENC_T_MS) no Core 1.
//         Atualiza a RPM e Posição simuladas (protegidas por mutex).
//         Notifica a 'task_spd_ctrl' para executar o controle.
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

        // Simula a física da esteira (atualiza rpm e posição)
        if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
            float err = g_belt.set_rpm - g_belt.rpm;
            g_belt.rpm += 0.01f * err; 
            g_belt.pos_mm += (g_belt.rpm / 60.0f) * ((float)ENC_T_MS / 1000.0f) * 100.0f;
            xSemaphoreGive(mutexBeltState);
        }

        // Simula carga de processamento
        int64_t t1 = esp_timer_get_time();
        cpu_tight_loop_us(700); 
        int64_t t2 = esp_timer_get_time();
        
        if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
            cpu_time.enc_time_us += (t2 - t1);
            xSemaphoreGive(mutexStats);
        }

        // Notifica a task de controle que há novos dados de sensor
        if (hCtrlNotify) {
            xTaskNotifyGive(hCtrlNotify);
        }

        // Aguarda próximo ciclo e conta misses (atrasos)
        if (xTaskDelayUntil(&next, T) != pdPASS) {
            if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
                stats.enc_misses++;
                xSemaphoreGive(mutexStats);
            }
        }
    }
}

// --- task_spd_ctrl ---
// Função: Executa o loop de controle de velocidade (PI).
// Design: Roda no Core 1 e é "acionada" por notificação da 'task_enc_sense'.
//         Calcula e aplica o esforço de controle (simulado) e
//         também processa eventos da fila qHMI (Touch HMI).
static void task_spd_ctrl(void *arg)
{
    float kp = 0.4f, ki = 0.1f, integ = 0.f;

    for (;;) {
        // Aguarda notificação da task ENC_SENSE (bloqueia até ser notificada)
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

        // Lógica de controle (PI)
        if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
            if (estop_ativo) integ = 0.f; // Zera integral se E-Stop ativo

            float err = g_belt.set_rpm - g_belt.rpm;
            integ += err * ((float)ENC_T_MS / 1000.0f);
            float u = kp * err + ki * integ;
            g_belt.rpm += 0.01f * u; 

            // Reseta a flag de E-stop se o setpoint for mudado para > 0
            if (estop_ativo && g_belt.set_rpm > 0.f) {
                estop_ativo = false;
            }
            xSemaphoreGive(mutexBeltState);
        }

        // Simula carga de processamento do controle
        int64_t t1 = esp_timer_get_time();
        cpu_tight_loop_us(800);
        int64_t t2 = esp_timer_get_time();

        if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
            cpu_time.ctrl_time_us += (t2 - t1);
            xSemaphoreGive(mutexStats);
        }

        // Processa eventos da IHM (Touch HMI) se houver algum na fila
        int64_t t_evt_us_hmi;
        if (xQueueReceive(qHMI, &t_evt_us_hmi, 0) == pdTRUE) {
            int64_t t_start = esp_timer_get_time();
            belt_state_t hmi_state;
            get_belt_state(&hmi_state);
            
            if (xSemaphoreTake(mutexStats, pdMS_TO_TICKS(10)) == pdTRUE) {
                int64_t latency_us = t_start - t_evt_us_hmi;
                stats.hmi_lat_total_us += latency_us;
                stats.hmi_lat_count++;
            
                printf("HMI: rpm=%.1f set=%.1f pos=%.1fmm (lat=%lld us)\n",
                    hmi_state.rpm, hmi_state.set_rpm, hmi_state.pos_mm, (long long)latency_us);
                
                // Simula carga extra do processamento da IHM
                int64_t t3 = esp_timer_get_time();
                cpu_tight_loop_us(300);
                int64_t t4 = esp_timer_get_time();
                cpu_time.ctrl_time_us += (t4 - t3);

                xSemaphoreGive(mutexStats);
            }
        } 	
    }
}

// --- task_sort_act ---
// Função: Processa eventos de "separação" (sort) da esteira.
// Design: Roda no Core 1 e fica bloqueada esperando eventos na 'qSort'.
//         Quando um evento chega, calcula a latência (ISR -> Tarefa),
//         simula o trabalho e atualiza as estatísticas (m,k)-firm.
static void task_sort_act(void *arg)
{
    sort_evt_t ev;
    for (;;) {
        // Bloqueia até um evento chegar na fila qSort
        if (xQueueReceive(qSort, &ev, portMAX_DELAY) == pdTRUE) {
            int64_t t_task_start = esp_timer_get_time();

            if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
                stats.sort_events++;
                xSemaphoreGive(mutexStats);
            }

            // Cálculo da latência (WCRT)
            int64_t latency_us = t_task_start - ev.t_evt_us;
            if (latency_us > sort_max_lat) sort_max_lat = latency_us;

            // Simula processamento do evento
            int64_t t1 = esp_timer_get_time();
            cpu_tight_loop_us(500);
            int64_t t2 = esp_timer_get_time();

            if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
                cpu_time.sort_time_us += (t2 - t1);
                stats.sort_lat_total_us += latency_us;
                stats.sort_lat_count++;
                xSemaphoreGive(mutexStats);
            }

            // Atualiza (m,k)-firm: checa se a latência cumpriu a deadline
            const int64_t deadline_us = 20000; // 20ms
            int met = latency_us <= deadline_us ? 1 : 0;
            mk_sort.count_true -= mk_sort.window[mk_sort.idx];
            mk_sort.window[mk_sort.idx] = met;
            mk_sort.count_true += met;
            mk_sort.idx = (mk_sort.idx + 1) % MK_K;

            printf("SORT_ACT: lat=%lld us met=%d\n", (long long)latency_us, met);
        }
    }
}


// --- task_safety ---
// Função: Processa eventos de Parada de Emergência (E-Stop).
// Design: Roda no Core 1 com prioridade máxima (PRIO_ESTOP).
//         Fica bloqueada esperando eventos na 'qSafety'.
//         Ao receber, para a esteira (zera RPM) imediatamente.
static void task_safety(void *arg)
{
    safety_evt_t ev;
    for (;;) {
        // Bloqueia até um evento chegar na fila qSafety
        if (xQueueReceive(qSafety, &ev, portMAX_DELAY) == pdTRUE) {
            int64_t t_task_start = esp_timer_get_time();

            if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
                stats.safety_events++;
                xSemaphoreGive(mutexStats);
            }

            // Cálculo da latência
            int64_t latency_us = t_task_start - ev.t_evt_us;
            if (latency_us > safety_max_lat) safety_max_lat = latency_us;

            // Simula carga de processamento da lógica de segurança
            int64_t t1 = esp_timer_get_time();
            cpu_tight_loop_us(800);
            int64_t t2 = esp_timer_get_time();

            if (xSemaphoreTake(mutexStats, portMAX_DELAY) == pdTRUE) {
                cpu_time.safety_time_us += (t2 - t1);
                stats.safety_lat_total_us += latency_us;
                stats.safety_lat_count++;
                xSemaphoreGive(mutexStats);
            }

            // Atualiza (m,k)-firm para a task de segurança
            const int64_t deadline_us = 30000; // 30ms
            int met = latency_us <= deadline_us ? 1 : 0;
            mk_safety.count_true -= mk_safety.window[mk_safety.idx];
            mk_safety.window[mk_safety.idx] = met;
            mk_safety.count_true += met;
            mk_safety.idx = (mk_safety.idx + 1) % MK_K;

            // AÇÃO DE SEGURANÇA: Para a esteira
            if (xSemaphoreTake(mutexBeltState, portMAX_DELAY) == pdTRUE) {
                g_belt.rpm = 0.f;
                g_belt.set_rpm = 0.f;
                estop_ativo = true;
                xSemaphoreGive(mutexBeltState);
            }

            printf("SAFETY: triggered lat=%lld us met=%d\n", (long long)latency_us, met);
        }
    }
}

// --- touch_cb ---
// Função: Rotina de Interrupção (ISR) dos Touch Pads.
// Design: Executa FORA do controle do RTOS (em modo de interrupção).
//         Deve ser o mais rápida possível.
//         Ela captura qual pad foi tocado, pega o timestamp,
//         e envia o evento para a fila (qSort, qSafety, qHMI) correta.
static void IRAM_ATTR touch_cb(void* arg)
{
    uint32_t pad_intr = touch_pad_get_status(); // quais pads dispararam
    touch_pad_clear_status();

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    int64_t now = esp_timer_get_time(); // Pega o timestamp

    portMUX_TYPE my_spinlock = portMUX_INITIALIZER_UNLOCKED;

    for (int pad = 0; pad < TOUCH_PAD_MAX; pad++) {
        if (pad_intr & (1 << pad)) {
            uint16_t val = 0;
            touch_pad_read_filtered(pad, &val);
            if (val < 200) { // Limiar de toque
                switch (pad) {
                    case TP_OBJ: {
                        // Evento de SORT_ACT
                        sort_evt_t ev = {.t_evt_us = now, .pad = TP_OBJ};
                        // Envia para a fila da task_sort_act
                        BaseType_t sent = xQueueSendFromISR(qSort, &ev, &xHigherPriorityTaskWoken);
                        if (sent != pdTRUE) { // Se a fila estiver cheia
                            portENTER_CRITICAL_ISR(&my_spinlock);
                            stats.sort_misses++; // Conta como 'miss'
                            portEXIT_CRITICAL_ISR(&my_spinlock);
                        }
                        break;
                    }
                    case TP_HMI: {
                        // Envia para a fila HMI (processada pela task_spd_ctrl)
                        xQueueSendFromISR(qHMI, &now, &xHigherPriorityTaskWoken);
                        break;
                    }
                    case TP_ESTOP: {
                        // Evento de E-Stop
                        safety_evt_t ev = {.t_evt_us = now, .pad = TP_ESTOP};
                        // Envia para a fila da task_safety
                        BaseType_t sent = xQueueSendFromISR(qSafety, &ev, &xHigherPriorityTaskWoken);
                        if (sent != pdTRUE) { // Se a fila estiver cheia
                            portENTER_CRITICAL_ISR(&my_spinlock);
                            stats.safety_misses++; // Conta como 'miss'
                            portEXIT_CRITICAL_ISR(&my_spinlock);
                        }
                        break;
                    }
                }
            }
        }
    }

    // Se uma task de prioridade maior foi acordada (ex: task_safety),
    // força uma troca de contexto imediata.
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR(); 
    }
}

// --- init_touch_pads ---
// Função: Configura os pinos de touch, filtros e registra a ISR 'touch_cb'.
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

// --- app_main ---
// Função: Ponto de entrada principal do firmware.
// Design: Inicializa hardware (NVS, WiFi, Touch, GPIO),
//         cria os Mutexes e Filas,
//         e finalmente cria e "pina" todas as tarefas RTOS em seus
//         respectivos núcleos de CPU (Core 0 ou Core 1).
void app_main(void)
{
    // Criação de semáforos e filas
    qSort = xQueueCreate(100, sizeof(sort_evt_t));
    qSafety = xQueueCreate(10, sizeof(safety_evt_t));
    qHMI = xQueueCreate(1, sizeof(int64_t));
    mutexBeltState = xSemaphoreCreateMutex();
    mutexStats = xSemaphoreCreateMutex();

    // Configura o LED
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0); // LED começa desligado

    // Inicializa periféricos
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    init_touch_pads();

    // --- Criação das Tarefas RTOS ---
    // Tarefas de Controle e Tempo Real são "pinadas" no Core 1
    xTaskCreatePinnedToCore(task_safety, "SAFETY", STK, NULL, PRIO_ESTOP, &hSAFE, 1);
    xTaskCreatePinnedToCore(task_enc_sense, "ENC_SENSE", STK, NULL, PRIO_ENC, &hENC, 1);
    xTaskCreatePinnedToCore(task_spd_ctrl, "SPD_CTRL", STK, NULL, PRIO_CTRL, &hCTRL, 1);
    hCtrlNotify = hCTRL; // Salva o handle para notificações
    xTaskCreatePinnedToCore(task_sort_act, "SORT_ACT", STK, NULL, PRIO_SORT, &hSORT, 1);
    
    // Tarefas de Rede e Sincronia (menos críticas em tempo) são "pinadas" no Core 0
    xTaskCreatePinnedToCore(task_time, "TIME_TASK", STK, NULL, PRIO_TIME, &hTIME, 0);
    xTaskCreatePinnedToCore(monitor_task, "MONITOR", STK, NULL, PRIO_MONITOR, &hMONITOR, 0);

    ESP_LOGI(TAG, "Tasks criadas e esteira inicializada com Touch pads");
}
