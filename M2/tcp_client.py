import socket
import json
import time

# --- Configuração ---
ESP_IP = "192.168.18.21"  # Coloque o IP do seu ESP32
PORT   = 10420            # Porta TCP definida no ESP32

print(f"--- CLIENTE TCP INTERATIVO ---")
print(f"Conectando a {ESP_IP}:{PORT}...")
print("Digite 'sair' para fechar.")
print("==================================================")

try:
    # Cria e conecta o socket TCP (com timeout de 5s)
    with socket.create_connection((ESP_IP, PORT), timeout=5) as s:
        # Recebe e imprime a mensagem de boas-vindas
        initial_data = s.recv(1024).decode()
        print(f"ESP> {initial_data.strip()}\n")
        
        rtt_samples = []

        while True:
            msg = input("Comando TCP > ").strip()
            if msg.lower() == 'sair':
                break
            if not msg:
                continue

            # Envia a mensagem com nova linha (\n) no final
            command_to_send = (msg.strip() + "\n").encode()
            
            # --- MEDIÇÃO DE RTT INÍCIO ---
            t_start = time.perf_counter()
            
            s.sendall(command_to_send)

            # Aguarda e recebe a resposta do servidor
            data = s.recv(1024).decode().strip()
            
            t_end = time.perf_counter()
            # --- MEDIÇÃO DE RTT FIM ---
            
            rtt_ms = (t_end - t_start) * 1000.0
            rtt_samples.append(rtt_ms)
            
            print(f"ESP> {data}")
            print(f"(RTT: {rtt_ms:.2f} ms)\n")


except ConnectionRefusedError:
    print(f"\nErro: Conexão recusada. Verifique o IP e se a ESP32 está em modo TCP.")
except socket.timeout:
    print(f"\nErro: Tempo limite de conexão esgotado.")
except Exception as e:
    print(f"\nUm erro ocorreu: {e}")

print("Fechando o socket TCP...")

# --- ANÁLISE FINAL DO RTT ---
if rtt_samples:
    print("\n================= ANÁLISE DE RTT (TCP) =================")
    print(f"Comandos enviados: {len(rtt_samples)}")
    print(f"Latência Mínima:   {min(rtt_samples):.2f} ms")
    print(f"Latência Média:    {sum(rtt_samples) / len(rtt_samples):.2f} ms")
    print(f"Latência Máxima:   {max(rtt_samples):.2f} ms")
    print("==========================================================")
