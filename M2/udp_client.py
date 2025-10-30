import socket
import time

# --- Configuração ---
ESP_IP = "192.168.18.21"
PORT   = 10421            # Porta de COMANDO UDP
PC_IP  = "192.168.18.10"  # IP desta máquina

# Este script vai usar uma porta diferente (ex: 10422) para enviar
# O Coletor de Logs (log_collector.py) deve estar rodando na 10421
CLIENT_PORT = 10422 

print(f"--- CLIENTE UDP INTERATIVO (COM RTT) ---")
print(f"Enviando para {ESP_IP}:{PORT}")
print("Digite 'sair' para fechar.")
print("AVISO: Certifique-se que 'log_collector.py' está rodando em outro terminal.")
print("==================================================")

rtt_samples = []

try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Define um timeout de 2 segundos para o socket
    sock.settimeout(2.0) 
    # Binda a uma porta específica para que a ESP32 possa responder
    sock.bind(("0.0.0.0", CLIENT_PORT)) 

    while True:
        msg = input("Comando UDP > ").strip()
        if msg.lower() == 'sair':
            break
        if not msg:
            continue

        command_to_send = (msg + "\n").encode()
        
        # --- MEDIÇÃO DE RTT INÍCIO ---
        t_start = time.perf_counter()
        
        sock.sendto(command_to_send, (ESP_IP, PORT))

        try:
            # Aguarda a resposta (bloqueante, com timeout)
            data, addr = sock.recvfrom(1024)
            
            t_end = time.perf_counter()
            # --- MEDIÇÃO DE RTT FIM ---
            
            rtt_ms = (t_end - t_start) * 1000.0
            rtt_samples.append(rtt_ms)
            
            print(f"ESP> {data.decode().strip()}")
            print(f"(RTT: {rtt_ms:.2f} ms)\n")
            
        except socket.timeout:
            print("Erro: Nenhuma resposta recebida do ESP32 (Timeout).\n")

except Exception as e:
    print(f"\nUm erro ocorreu: {e}")

print("Fechando o socket UDP...")
sock.close()

# --- ANÁLISE FINAL DO RTT ---
if rtt_samples:
    print("\n================= ANÁLISE DE RTT (UDP) =================")
    print(f"Comandos enviados: {len(rtt_samples)}")
    print(f"Latência Mínima:   {min(rtt_samples):.2f} ms")
    print(f"Latência Média:    {sum(rtt_samples) / len(rtt_samples):.2f} ms")
    print(f"Latência Máxima:   {max(rtt_samples):.2f} ms")
    print("==========================================================")
