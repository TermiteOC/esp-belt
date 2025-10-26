import socket, json, threading, time, select

ESP_IP = "IP da esp (log vscode)"
PORT = 10421
PC_IP = "IP do ipconfig"

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((PC_IP, PORT))
sock.setblocking(False)

def display_full_report(data):
    try:
        j = json.loads(data)
        if j.get("type") != "full_report": return
        print(f"\n=== REPORT ({time.strftime('%H:%M:%S')}) ===")
        print(f"RPM={j['rpm']:.1f} | SET={j['set_rpm']:.1f} | POS={j['pos']:.1f}mm | CPU={j['cpu_pct']:.1f}%")
    except:
        pass

def listener():
    """Thread que escuta relatórios e respostas da ESP."""
    while True:
        try:
            r, _, _ = select.select([sock], [], [], 0.2)
            if r:
                data, _ = sock.recvfrom(2048)
                display_full_report(data.decode().strip())
        except Exception:
            pass

def sender():
    """Thread que lê comandos do teclado e envia para ESP."""
    while True:
        cmd = input("\nDigite comando (status, estop_on, estop_reset, set_rpm X, ping, sair): ").strip()
        if cmd.lower() in ("exit", "sair"):
            print("Encerrando cliente...")
            sock.close()
            break
        sock.sendto((cmd + "\n").encode(), (ESP_IP, PORT))

def ping_test(n=5):
    for i in range(n):
        t1 = time.time()
        sock.sendto(b"ping\n", (ESP_IP, PORT))
        r, _, _ = select.select([sock], [], [], 2)
        if r:
            data, _ = sock.recvfrom(1024)
            t2 = time.time()
            print(f"Ping {i+1}: RTT={(t2-t1)*1000:.2f} ms -> {data.decode().strip()}")
        else:
            print(f"Ping {i+1}: sem resposta")
        time.sleep(1)

print(f"Cliente UDP ativo em {PC_IP}:{PORT} → ESP {ESP_IP}:{PORT}")
threading.Thread(target=listener, daemon=True).start()
sender()
