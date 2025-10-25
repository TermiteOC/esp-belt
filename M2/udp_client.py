import socket
import json
import time

ESP_IP = "IP da esp (esp_netif_handler)"  # IP CORRIGIDO
PORT = 10421

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5) 

print(f"Cliente UDP ativo. Endereço do Servidor ESP: {ESP_IP}:{PORT}")

# COMANDOS CORRIGIDOS E NO FORMATO ESPERADO PELO C:
commands_to_test = ["status", "estop_on", "set_rpm 150.0", "estop_reset"]

try:
    for msg in commands_to_test:
        # Nota: O script Python já adiciona '\n' no final
        s.sendto((msg + "\n").encode(), (ESP_IP, PORT)) 
        print(f"\nPC > Sent: {msg}")
        try:
            data, server_addr = s.recvfrom(1024)
            data = data.decode()
            
            print(f"ESP> Received from {server_addr}: {data.strip()}")
            try:
                print("JSON:", json.loads(data))
            except json.JSONDecodeError:
                pass

        except socket.timeout:
            print("ESP> TIMEOUT: Nenhuma resposta recebida do Servidor UDP.")
        time.sleep(1)
        
finally:
    print("Fechando o socket UDP...")
    s.close()