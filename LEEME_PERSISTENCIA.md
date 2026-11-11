# BIOSENSE - SD en el TX + persistencia

## Cableado de la SD (modulo microSD -> Heltec V3 del TX)
GND->GND | VCC->5V | MISO->GPIO45 | CLK->GPIO47 | MOSI->GPIO48 | CS->GPIO26
(La SD debe estar en FAT32.)

## Como funciona
TX: lee sensores -> escribe la lectura en /datos.txt (SD) -> la envia por LoRa -> espera "A,<seq>" del RX.
    Si llega el ACK la da por entregada. Si no, queda pendiente y se reenvia (4 por ciclo, las mas
    viejas primero) cuando el enlace vuelve. El puntero de pendientes se guarda en /ack.txt, asi que
    sobrevive a reinicios y cortes de energia.
RX: valida la trama -> la publica por MQTT a la Raspberry -> SOLO si la entrego, responde el ACK.
    Si no hay WiFi/broker no publica ni confirma: el TX la reenvia despues.
Pi: ubica cada lectura por la hora que puso el TX (RTC), descarta repetidas y sube a ThingsBoard
    una mediana por minuto con su hora real. Si no hay internet, los minutos pendientes se suben
    solos al volver. Lo que llega atrasado corrige su minuto.

Trama LoRa:  SEQ,AAAA-MM-DD HH:MM:SS,TEMP,TDS,PH,OD,SST
MQTT al Pi:  la misma + ,RSSI,SNR

## Que copiar
nodo_tx/main/*  -> reemplaza/agrega en tu nodo_tx/main  (CAMBIA config.h: lo traigo completo con tus valores)
nodo_rx/main/main.c -> reemplaza el main.c del RX (sd_logger.* y lo demas no se tocan)
raspberry/biosense_edge.py -> reemplaza el de la Pi y reinicia el servicio

## Ajustar la hora del DS3231 (una sola vez)
En nodo_tx/main/config.h pon RTC_SET_FROM_BUILD 1, compila y flashea; luego ponlo en 0 y flashea otra vez.
Queda con la hora de tu PC (Colombia). La Pi asume America/Bogota; si lo ajustas en UTC define TX_TZ=UTC
en biosense.env.

## Pruebas
1. Sin RX encendido: el monitor del TX debe decir "SIN confirmacion" y subir "pendientes en SD".
2. Enciende el RX: deben aparecer "Reenviada SEQ ..." y bajar los pendientes a 0.
3. Apaga el WiFi del router un rato y enciendelo: pasa lo mismo.
4. Desconecta internet de la Pi: el log dice "pendiente(s)" y al volver sube los minutos.

## Alertas por Discord (en la Raspberry)
1. Discord: tu servidor > Editar canal > Integraciones > Webhooks > Nuevo webhook > Copiar URL.
2. En la Pi, en ~/biosense_edge/biosense.env agrega:  DISCORD_WEBHOOK_URL=<la URL>   (chmod 600; no la compartas)
3. Copia alerts.py y biosense_edge.py a ~/biosense_edge/ y: sudo systemctl restart biosense-edge
   Debe llegar el mensaje "Monitor BIOSENSE iniciado".
Umbrales (ya cargados en alerts.py): temperatura, TDS, pH, OD y SST segun tu tabla.
Se avisa si el estado se mantiene 2 minutos seguidos, al volver a NORMAL, cada 30 min si sigue CRITICO,
si un sensor no da lectura valida y si no llegan datos del TX en 5 minutos (ajustable en biosense.env).
