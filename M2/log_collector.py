import socket, select, json, statistics, os, sys, threading

PORT = 10421
LOG_FILE_NAME = "logs_esp32.txt"

# Lista para guardar os relatórios para análise
relatorios = []
# Lock para proteger a lista 'relatorios' entre as threads
relatorios_lock = threading.Lock()
# Flag para parar a thread de escuta
listener_running = True

# Limpa o arquivo de log antigo ao iniciar
if os.path.exists(LOG_FILE_NAME):
    os.remove(LOG_FILE_NAME)

# Abre o arquivo de log globalmente
try:
    log_file = open(LOG_FILE_NAME, "a", encoding="utf-8")
except Exception as e:
    print(f"Erro ao abrir arquivo de log: {e}")
    sys.exit(1)

# --- Thread de Escuta ---
# Esta thread roda em segundo plano e APENAS escuta a rede
def listener_thread(sock):
    global relatorios, listener_running
    while listener_running:
        try:
            # Espera por dados no socket (timeout de 1 segundo)
            r, _, _ = select.select([sock], [], [], 1.0)
            
            if r:
                # Dados do socket (ESP32)
                data, _ = sock.recvfrom(4096)
                data_str = data.decode(errors="ignore").strip()
                
                if data_str:
                    print(f"\n--- Relatório de Performance Recebido (Seq: {json.loads(data_str).get('seq')}) ---")
                    log_file.write(data_str + "\n")
                    log_file.flush()
                    try:
                        j = json.loads(data_str)
                        if j.get("type") == "monitor_report":
                            with relatorios_lock:
                                relatorios.append(j)
                    except json.JSONDecodeError:
                        pass
        except Exception as e:
            if listener_running:
                print(f"\nErro na thread de escuta: {e}")
    
    print("Thread de escuta encerrada.")

# --- Função de Análise ---
def analisar():
    global relatorios
    if not relatorios:
        print("Nenhum relatório de performance recebido ainda.")
        return

    print("\n===== ANÁLISE DE PERFORMANCE (CPU/WCRT) =====")
    
    with relatorios_lock:
        relatorios_copia = list(relatorios)
    
    # Filtra relatórios que têm dados de latência válidos ( > 0)
    lat_sort = [r["sort"]["max_lat_us"] for r in relatorios_copia if r["sort"]["events"] > 0]
    lat_safety = [r["safety"]["max_lat_us"] for r in relatorios_copia if r["safety"]["events"] > 0]
    cpu = [r["cpu_usage_pct"] for r in relatorios_copia]

    def resumo(nome, dados):
        if dados:
            print(f"{nome} (WCRT/HWM): média={statistics.mean(dados):.2f} µs | máx={max(dados):.2f} | mediana={statistics.median(dados):.2f}")
            if len(dados) >= 10:
                p95 = statistics.quantiles(dados, n=100)[94] # 95º
                p99 = statistics.quantiles(dados, n=100)[98] # 99º
                print(f"   Percentis: P95={p95:.2f} µs | P99={p99:.2f} µs")
        else:
            print(f"{nome}: Nenhum evento registrado ainda.")

    resumo("SORT_ACT", lat_sort)
    resumo("SAFETY", lat_safety)
    
    if cpu:
        print(f"CPU usage: média = {statistics.mean(cpu):.2f}% (máx {max(cpu):.2f}%)")
    print("==================================================")


# --- Bloco Principal (Execução) ---
try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", PORT))
    sock.setblocking(False)

    print(f"--- COLETOR DE LOGS ATIVO na porta {PORT} ---")
    print(f"Salvando relatórios de performance em {LOG_FILE_NAME}")
    print("Deixe este terminal rodando em segundo plano durante os testes.")
    
    # Inicia a thread de escuta
    t = threading.Thread(target=listener_thread, args=(sock,))
    t.daemon = True
    t.start()

    # Loop principal para escutar o teclado
    while True:
        cmd = input("\nDigite 'analisar' (para ver o resumo) ou 'sair' (para fechar):\n")
        if cmd.lower() == 'analisar':
            analisar()
        elif cmd.lower() == 'sair':
            print("Encerrando...")
            break
        else:
            print("Comando desconhecido.")

except KeyboardInterrupt:
    print("\n(Ctrl+C) Encerrando...")

finally:
    # Sinaliza para a thread parar
    listener_running = False
    # Espera a thread terminar
    if 't' in locals() and t.is_alive():
        t.join(timeout=1.0)
        
    if 'sock' in locals():
        sock.close()
    if 'log_file' in locals():
        log_file.close()

    print("Coletor de logs encerrado.")
