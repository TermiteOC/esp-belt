import socket
import time

# --- Configuração ---
ESP_IP = "192.168.18.21"
PORT   = 10421            # Porta de COMANDO UDP no ESP32
PC_IP  = "192.168.18.10"  # IP desta máquina (PC)

# Porta que este cliente usará para ouvir a resposta
CLIENT_PORT = 10422 

print(f"--- CLIENTE UDP INTERATIVO (COM RTT) ---")
print(f"Enviando para {ESP_IP}:{PORT}")
print(f"Ouvindo respostas na porta {CLIENT_PORT}")
print("Digite 'sair' para fechar.")
print("==================================================")

rtt_samples = [] # Lista para guardar as medições de latência

try:
    # Função: Cria o socket UDP
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # Define um timeout de 2 segundos para o recebimento (recvfrom)
    # Se o ESP32 não responder em 2s, o recvfrom dará um erro 'socket.timeout'.
    sock.settimeout(2.0) 
    
    # Função: Faz o "bind" do socket a uma porta local.
    # Isso é essencial para que o ESP32 saiba para qual porta enviar a resposta.
    sock.bind(("0.0.0.0", CLIENT_PORT)) 

    # Loop principal para enviar comandos interativos
    while True:
        msg = input("Comando UDP > ").strip()
        if msg.lower() == 'sair':
            break
        if not msg:
            continue

        command_to_send = (msg + "\n").encode()
        
        # --- MEDIÇÃO DE RTT INÍCIO ---
        # Marca o tempo exato ANTES de enviar o comando
        t_start = time.perf_counter()
        
        # Envia o pacote UDP para o IP e Porta do ESP32
        sock.sendto(command_to_send, (ESP_IP, PORT))

        try:
            # Função: Aguarda (bloqueante) pela resposta do ESP32.
            # Se estourar o timeout de 2s, vai para o 'except socket.timeout'.
            data, addr = sock.recvfrom(1024)
            
            # Marca o tempo exato DEPOIS de receber a resposta
            t_end = time.perf_counter()
            # --- MEDIÇÃO DE RTT FIM ---
            
            # Calcula o Round-Trip Time (RTT) em milissegundos
            rtt_ms = (t_end - t_start) * 1000.0
            rtt_samples.append(rtt_ms)
            
            print(f"ESP> {data.decode().strip()}")
            print(f"(RTT: {rtt_ms:.2f} ms)\n")
            
        except socket.timeout:
            # Tratamento de erro para pacotes perdidos (timeout)
            print("Erro: Nenhuma resposta recebida do ESP32 (Timeout).\n")

except Exception as e:
    print(f"\nUm erro ocorreu: {e}")

print("Fechando o socket UDP...")
sock.close()

# --- ANÁLISE FINAL DO RTT ---
# Função: Calcula e exibe estatísticas de latência (Mín, Média, Máx).
if rtt_samples:
    print("\n================= ANÁLISE DE RTT (UDP) =================")
    print(f"Comandos enviados: {len(rtt_samples)}")
    print(f"Latência Mínima:   {min(rtt_samples):.2f} ms")
    print(f"Latência Média:    {sum(rtt_samples) / len(rtt_samples):.2f} ms")
    print(f"Latência Máxima:   {max(rtt_samples):.2f} ms")
    print("==========================================================")
