#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/touch_pad.h"
#include "esp_err.h"

#define TAG "ESTEIRA"

// ===== Mapas dos touch pads =====
// TP_OBJ -> Touch B: detecta objeto (para acionar desviador)
// TP_HMI -> Touch C: solicita atualização de HMI/telemetria
// TP_ESTOP -> Touch D: E-stop (emergência)
#define TP_OBJ   TOUCH_PAD_NUM0   // GPIO4
#define TP_HMI   TOUCH_PAD_NUM3   // GPIO15
#define TP_ESTOP TOUCH_PAD_NUM7   // GPIO27

// ===== Configurações de prioridade e stack =====
#define ENC_T_MS   5     // período task enc_sense (ms)
#define PRIO_ESTOP 5
#define PRIO_ENC   4
#define PRIO_CTRL  3
#define PRIO_SORT  3
#define PRIO_REP   2     // task de relatório
#define STK        4096  // tamanho de stack (bytes)

// ===== Handles das tasks, filas e semáforos =====
static TaskHandle_t hENC = NULL, hCTRL = NULL, hSORT = NULL, hSAFE = NULL, hREP = NULL;
static TaskHandle_t hCtrlNotify = NULL;
static QueueHandle_t qSort = NULL;    // fila de eventos SORT_ACT
static QueueHandle_t qSafety = NULL;  // fila de eventos SAFETY
static SemaphoreHandle_t semEStop = NULL; // semáforo para E-stop
static SemaphoreHandle_t semHMI = NULL;   // semáforo para HMI
static volatile bool estop_ativo = false; // flag de E-stop ativo

// ===== Estado da esteira =====
typedef struct {
    float rpm;      // velocidade atual (rpm)
    float pos_mm;   // posição em mm
    float set_rpm;  // setpoint de rpm
} belt_state_t;

static belt_state_t g_belt = {0.f, 0.f, 120.f};

// Estruturas para eventos
typedef struct {
    int64_t t_evt_us; // timestamp do evento (para latência)
    int pad;          // qual touch pad
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

    int64_t sort_lat_total_us;   // soma latência SORT
    uint32_t sort_lat_count;     // qtd de eventos medidos

    int64_t safety_lat_total_us; // soma latência Safety
    uint32_t safety_lat_count;

    int64_t hmi_lat_total_us;    // soma latência HMI
    uint32_t hmi_lat_count;
} stats = {0};

// Contadores de tempo ocupado da CPU (microsegundos)
static struct {
    int64_t enc_time_us;
    int64_t ctrl_time_us;
    int64_t sort_time_us;
    int64_t safety_time_us;
} cpu_time = {0};

// ===== Função busy loop determinístico =====
// Simula carga de CPU previsível (~WCET)
static inline void cpu_tight_loop_us(uint32_t us)
{
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < (int64_t)us) {
        asm volatile("nop"); // instrução "no operation"
    }
}

// ===== Task: Encoders =====
static void task_enc_sense(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    TickType_t T = pdMS_TO_TICKS(ENC_T_MS);
    if (T == 0) T = 1;

    for (;;) {
        stats.enc_samples++;

        // Simula controle da velocidade da esteira
        float err = g_belt.set_rpm - g_belt.rpm;
        g_belt.rpm += 0.05f * err;
        g_belt.pos_mm += (g_belt.rpm / 60.0f) * ((float)ENC_T_MS / 1000.0f) * 100.0f;

        int64_t t1 = esp_timer_get_time();
        cpu_tight_loop_us(700); // simula carga determinística
        int64_t t2 = esp_timer_get_time();
        cpu_time.enc_time_us += (t2 - t1);

        // Notifica task de controle
        if (hCtrlNotify) {
            xTaskNotifyGive(hCtrlNotify);
        }

        // Aguarda próximo ciclo
        if (xTaskDelayUntil(&next, T) != pdPASS) {
            stats.enc_misses++;
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
            stats.ctrl_misses++;
            continue;
        }
        stats.ctrl_runs++;

        if (estop_ativo) integ = 0.f;

        // Controle PI simples
        float err = g_belt.set_rpm - g_belt.rpm;
        integ += err * ((float)ENC_T_MS / 1000.0f);
        float u = kp * err + ki * integ;
        g_belt.rpm += 0.05f * u;

        int64_t t1 = esp_timer_get_time();
        cpu_tight_loop_us(800);
        int64_t t2 = esp_timer_get_time();
        cpu_time.ctrl_time_us += (t2 - t1);

        // Trecho soft HMI
        if (xSemaphoreTake(semHMI, 0) == pdTRUE) {
            int64_t t_hmi = esp_timer_get_time();
            printf("HMI: rpm=%.1f set=%.1f pos=%.1fmm (t=%lld)\n",
                   g_belt.rpm, g_belt.set_rpm, g_belt.pos_mm, (long long)t_hmi);
            
            int64_t t3 = esp_timer_get_time();
            cpu_tight_loop_us(300);
            int64_t t4 = esp_timer_get_time();
            cpu_time.ctrl_time_us += (t4 - t3);
        }

        // Reset E-stop se necessário
        if (estop_ativo && g_belt.set_rpm > 0.f) {
            estop_ativo = false;
        }
    }
}

// ===== Task: SORT_ACT =====
static void task_sort_act(void *arg)
{
    sort_evt_t ev;
    for (;;) {
        if (xQueueReceive(qSort, &ev, portMAX_DELAY) == pdTRUE) {
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
        }
    }
}

// ===== Task: SAFETY (E-stop) =====
static void task_safety(void *arg)
{
    safety_evt_t ev;
    for (;;) {
        if (xQueueReceive(qSafety, &ev, portMAX_DELAY) == pdTRUE) {
            stats.safety_events++;
            int64_t t_start = esp_timer_get_time();

            // Latência ISR -> task
            int64_t latency_us = t_start - ev.t_evt_us;
            stats.safety_lat_total_us += latency_us;
            stats.safety_lat_count++;

            // Ação de emergência
            g_belt.rpm = 0.f;
            estop_ativo = true;

            int64_t t1 = esp_timer_get_time();
            cpu_tight_loop_us(800);
            int64_t t2 = esp_timer_get_time();
            cpu_time.safety_time_us += (t2 - t1);

            printf("SAFETY: E-STOP acionado @ %lld -> motor parado (lat=%lld us)\n",
                   (long long)t_start, (long long)latency_us);
        }
    }
}

// ===== Task: REPORT =====
static void task_report(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    const TickType_t T = pdMS_TO_TICKS(60000); // 10 segundos

    for (;;) {
        vTaskDelayUntil(&next, T);

        int64_t t_now = esp_timer_get_time();

        // Calcula diferenças desde o último relatório
        uint32_t enc_samples_now = stats.enc_samples - last_stats.enc_samples;
        uint32_t enc_misses_now  = stats.enc_misses - last_stats.enc_misses;
        uint32_t ctrl_runs_now   = stats.ctrl_runs - last_stats.ctrl_runs;
        uint32_t ctrl_misses_now = stats.ctrl_misses - last_stats.ctrl_misses;
        uint32_t sort_events_now = stats.sort_events - last_stats.sort_events;
        uint32_t sort_misses_now = stats.sort_misses - last_stats.sort_misses;
        uint32_t safety_events_now = stats.safety_events - last_stats.safety_events;
        uint32_t safety_misses_now = stats.safety_misses - last_stats.safety_misses;

        // Atualiza last_stats
        last_stats.enc_samples = stats.enc_samples;
        last_stats.enc_misses  = stats.enc_misses;
        last_stats.ctrl_runs   = stats.ctrl_runs;
        last_stats.ctrl_misses = stats.ctrl_misses;
        last_stats.sort_events = stats.sort_events;
        last_stats.sort_misses = stats.sort_misses;
        last_stats.safety_events = stats.safety_events;
        last_stats.safety_misses = stats.safety_misses;

        // Cálculo de porcentagem de misses
        float enc_miss_pct   = 100.0f * enc_misses_now   / (enc_samples_now   + enc_misses_now);
        float ctrl_miss_pct  = 100.0f * ctrl_misses_now  / (ctrl_runs_now     + ctrl_misses_now);

        float sort_miss_pct = 0.0f;
        if ((sort_events_now + sort_misses_now) != 0)
            sort_miss_pct = 100.0f * sort_misses_now / (sort_events_now + sort_misses_now);

        float safety_miss_pct = 0.0f;
        if ((safety_events_now + safety_misses_now) != 0)
            safety_miss_pct = 100.0f * safety_misses_now / (safety_events_now + safety_misses_now);

        int64_t avg_sort_lat = stats.sort_lat_count ? stats.sort_lat_total_us / stats.sort_lat_count : 0;
        int64_t avg_safety_lat = stats.safety_lat_count ? stats.safety_lat_total_us / stats.safety_lat_count : 0;

        // Uso aproximado da CPU
        float cpu_usage = (cpu_time.enc_time_us +
                           cpu_time.ctrl_time_us +
                           cpu_time.sort_time_us +
                           cpu_time.safety_time_us) * 100.0f / 10000000.0f; // 10s em micros

        // Reset tempos
        cpu_time.enc_time_us = cpu_time.ctrl_time_us = cpu_time.sort_time_us = cpu_time.safety_time_us = 0;

        // Impressão do relatório
        ESP_LOGI(TAG, "=== REPORT (%lld us) ===", (long long)t_now);
        ESP_LOGI(TAG, "ENC_SENSE: samples=%u misses=%u", enc_samples_now, enc_misses_now);
        ESP_LOGI(TAG, "SPD_CTRL : runs=%u misses=%u", ctrl_runs_now, ctrl_misses_now);
        ESP_LOGI(TAG, "SORT_ACT : events=%u misses=%u", sort_events_now, sort_misses_now);
        ESP_LOGI(TAG, "SORT_ACT latency (avg) = %lld us", (long long)avg_sort_lat);
        ESP_LOGI(TAG, "SAFETY   : events=%u misses=%u", safety_events_now, safety_misses_now);
        ESP_LOGI(TAG, "SAFETY latency (avg) = %lld us", (long long)avg_safety_lat);
        ESP_LOGI(TAG, "Belt state: rpm=%.1f set=%.1f pos=%.1fmm",
                 g_belt.rpm, g_belt.set_rpm, g_belt.pos_mm);
        ESP_LOGI(TAG, "CPU usage (aprox) = %.1f%%", cpu_usage);
        ESP_LOGI(TAG, "Miss pct: ENC=%.2f%% CTRL=%.2f%% SORT=%.2f%% SAFETY=%.2f%%",
                 enc_miss_pct, ctrl_miss_pct, sort_miss_pct, safety_miss_pct);
        ESP_LOGI(TAG, "============================");

        stats.sort_lat_total_us = 0;
        stats.sort_lat_count = 0;
        stats.safety_lat_total_us = 0;
        stats.safety_lat_count = 0;
    }
}

// ===== ISR Touch =====
static void IRAM_ATTR touch_cb(void* arg)
{
    uint32_t pad_intr = touch_pad_get_status(); // quais pads dispararam
    touch_pad_clear_status();

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    int64_t now = esp_timer_get_time();

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
                        if (sent != pdTRUE) stats.sort_misses++;
                        break;
                    }
                    case TP_HMI:
                        // HMI
                        xSemaphoreGiveFromISR(semHMI, &xHigherPriorityTaskWoken);
                        break;
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
    semEStop = xSemaphoreCreateBinary();
    semHMI = xSemaphoreCreateBinary();
    qSort = xQueueCreate(100, sizeof(sort_evt_t));
    qSafety = xQueueCreate(10, sizeof(safety_evt_t));

    if (!semEStop || !semHMI || !qSort || !qSafety) {
        ESP_LOGE(TAG, "Falha ao criar semáforos/filas");
        return;
    }

    init_touch_pads();

    // Criação de tasks
    xTaskCreatePinnedToCore(task_safety, "SAFETY", STK, NULL, PRIO_ESTOP, &hSAFE, 0);
    xTaskCreatePinnedToCore(task_enc_sense, "ENC_SENSE", STK, NULL, PRIO_ENC, &hENC, 0);
    xTaskCreatePinnedToCore(task_spd_ctrl, "SPD_CTRL", STK, NULL, PRIO_CTRL, &hCTRL, 0);
    hCtrlNotify = hCTRL; // handle para notificações
    xTaskCreatePinnedToCore(task_sort_act, "SORT_ACT", STK, NULL, PRIO_SORT, &hSORT, 0);
    xTaskCreatePinnedToCore(task_report, "REPORT", STK, NULL, PRIO_REP, &hREP, 0);

    ESP_LOGI(TAG, "Tasks criadas e esteira inicializada com Touch pads");
}