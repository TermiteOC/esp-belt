import socket, threading, time, select, json, statistics

ESP_IP = "10.15.0.235"
PORT = 10421
PC_IP = "10.15.0.146"

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PORT))
sock.setblocking(False)

log_file = open("logs_esp32.txt", "a", encoding="utf-8")
relatorios = []

def display_full_report(data: str):
    if data.strip():
        print("\n--- Log recebido da ESP32 ---")
        print(data)
        log_file.write(data + "\n")
        log_file.flush()
        try:
            j = json.loads(data)
            if j.get("type") == "monitor_report":
                relatorios.append(j)
        except json.JSONDecodeError:
            pass

def listener():
    """Thread que escuta relatórios e respostas da ESP."""
    while True:
        try:
            r, _, _ = select.select([sock], [], [], 0.2)
            if r:
                data, _ = sock.recvfrom(4096)
                display_full_report(data.decode(errors="ignore").strip())
        except Exception as e:
            print("Erro no listener:", e)

def analisar():
    if not relatorios:
        print("Nenhum relatório recebido ainda.")
        return

    lat_sort = [r["sort"]["avg_lat_us"] for r in relatorios]
    lat_safety = [r["safety"]["avg_lat_us"] for r in relatorios]
    cpu = [r["cpu_usage_pct"] for r in relatorios]

    print("\n===== ANÁLISE DOS RELATÓRIOS =====")
    def resumo(nome, dados):
        if dados:
            print(f"{nome}: média={statistics.mean(dados):.2f} µs | máx={max(dados):.2f} | mediana={statistics.median(dados):.2f}")
            if len(dados) > 10:
                print(f"   95º percentil = {statistics.quantiles(dados, n=100)[94]:.2f} µs")

    resumo("SORT_ACT", lat_sort)
    resumo("SAFETY", lat_safety)
    print(f"CPU usage médio = {statistics.mean(cpu):.2f}% (máx {max(cpu):.2f}%)")

def sender():
    """Thread que lê comandos do teclado e envia para ESP."""
    while True:
        cmd = input("\nDigite comando (status, estop_on, estop_reset, set_rpm X, ping, analisar, sair): ").strip()
        if cmd.lower() in ("exit", "sair"):
            print("Encerrando cliente...")
            sock.close()
            log_file.close()
            break
        elif cmd.lower() == "analisar":
            analisar()
        else:
            sock.sendto((cmd + "\n").encode(), (ESP_IP, PORT))

print(f"Cliente UDP ativo em {PC_IP}:{PORT} → ESP {ESP_IP}:{PORT}")
threading.Thread(target=listener, daemon=True).start()
sender()
