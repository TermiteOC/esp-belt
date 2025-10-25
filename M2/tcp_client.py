import socket
import json
import time

# --- Configuração ---
ESP_IP = "IP da esp (esp_netif_handler)"  # Coloque o IP do seu ESP32
PORT   = 10420            # Porta TCP definida no ESP32

# Lista dos comandos a serem testados
COMMANDS_TO_TEST = [
    "status",               # Pede o estado atual da esteira
    "estop_on",             # Aciona o E-stop
    "status",               # Verifica o estado (RPM deve ser 0.0, estop_ativo=true)
    "set_rpm 150.0",        # Define um novo setpoint, o que deve resetar a flag do E-stop
    "status",               # Verifica o estado (RPM deve estar subindo)
    "set_rpm 120.0",        # Define setpoint de volta para o padrão
    "status",               # Verifica o estado (RPM deve estar descendo)
    "estop_reset",          # Comando auxiliar (set_rpm 120.0)
    "status",               # Última verificação
]

print(f"Cliente TCP ativo. Endereço do Servidor ESP: {ESP_IP}:{PORT}")
print("==========================================================")

try:
    # Cria e conecta o socket TCP (com timeout de 5s)
    with socket.create_connection((ESP_IP, PORT), timeout=5) as s:
        # Recebe e imprime a mensagem de boas-vindas do servidor (opcional)
        initial_data = s.recv(1024).decode()
        print(f"ESP> Conexão Estabelecida. Boas-vindas:\n{initial_data.strip()}")

        for msg in COMMANDS_TO_TEST:
            # Envia a mensagem com nova linha (\n) no final
            command_to_send = (msg.strip() + "\n").encode()
            s.sendall(command_to_send)
            print(f"PC > Sent: {msg}")

            # Aguarda e recebe a resposta do servidor
            data = s.recv(1024).decode().strip()
            print(f"ESP> Received: {data}")

            # Tenta decodificar e imprimir o JSON
            try:
                # O servidor ESP32 envia JSON, vamos decodificá-lo
                json_data = json.loads(data)
                print("JSON:", json_data)
            except json.JSONDecodeError:
                print("JSON: Não decodificado (resposta não JSON ou erro de formatação).")
                pass
            
            # Pequena pausa para simular a interação humana e observar as rampas de RPM
            time.sleep(1) 

except ConnectionRefusedError:
    print(f"\nErro: Conexão recusada para {ESP_IP}:{PORT}. Verifique se o ESP32 está conectado e se o Servidor TCP está rodando.")
except socket.timeout:
    print(f"\nErro: Tempo limite de conexão esgotado para {ESP_IP}:{PORT}. Verifique o IP e a conectividade de rede.")
except Exception as e:
    print(f"\nUm erro ocorreu: {e}")

print("Fechando o socket TCP...")