
# Firmware — Coleira Inteligente

Firmware desenvolvido para a **coleira inteligente para pets**, utilizando o microcontrolador ESP32-C3. O sistema permite configurar a conexão Wi-Fi por Bluetooth Low Energy (BLE), reconhecer o dispositivo no Supabase, monitorar a localização GPS, medir a temperatura e detectar sons persistentes.

## Funcionalidades

- Configuração inicial da coleira pelo aplicativo via BLE.
- Conexão com redes Wi-Fi de 2,4 GHz.
- Reconhecimento e autenticação do dispositivo no Supabase.
- Envio periódico da localização GPS.
- Verificação de áreas cadastradas e detecção de saída de área.
- Leitura e envio da temperatura.
- Detecção de sons persistentes e envio de alertas.
- Armazenamento das configurações na memória não volátil.
- Comandos de diagnóstico pelo Monitor Serial.
- Reconexão automática com Wi-Fi e tentativas de reconhecimento do dispositivo.

## Hardware utilizado

| Componente | Função |
|---|---|
| ESP32-C3 Dev Module | Microcontrolador principal |
| GPS NEO-6M | Obtenção de latitude e longitude |
| DS18B20 | Medição de temperatura |
| Sensor de som analógico | Detecção de sons persistentes |
| Fonte de alimentação | Alimentação do circuito |

## Ligações dos componentes

As conexões utilizadas pelo firmware são:

| Componente | Pino do componente | GPIO do ESP32-C3 |
|---|---|---|
| GPS NEO-6M | TX | GPIO 4 (RX do ESP32-C3) |
| GPS NEO-6M | RX | GPIO 3 (TX do ESP32-C3) |
| DS18B20 | DATA | GPIO 1 |
| Sensor de som | Saída analógica (AO) | GPIO 0 |
| Todos os componentes | GND | GND |

### Observações sobre a montagem

- O DS18B20 utiliza comunicação OneWire.
- Utilize um resistor pull-up de aproximadamente 4,7 kΩ entre DATA do DS18B20 e 3,3 V.
- Todos os componentes devem compartilhar o mesmo GND.
- Confira a tensão de alimentação e os níveis lógicos de cada módulo antes de conectá-los ao ESP32-C3.
- Não aplique 5 V diretamente a um GPIO do ESP32-C3.
- A alimentação deve ser adequada ao consumo dos módulos e à bateria utilizada na coleira.

## Ambiente de desenvolvimento

O firmware pode ser desenvolvido e compilado utilizando a Arduino IDE com o pacote de placas ESP32 instalado.

### Placa

Selecione a placa correspondente ao seu hardware:

`ESP32C3 Dev Module`

Confira também a porta serial correta antes de carregar o firmware.

### Bibliotecas utilizadas

Instale ou disponibilize as bibliotecas necessárias ao código:

- `Preferences` — armazenamento persistente de configurações.
- `WiFi` — conexão com redes sem fio.
- `WiFiClientSecure` — comunicação HTTPS.
- `HTTPClient` — requisições HTTP/HTTPS.
- `BLEDevice`, `BLEServer`, `BLEUtils` e `BLE2902` — comunicação Bluetooth Low Energy.
- `TinyGPSPlus` — interpretação dos dados do GPS.
- `OneWire` — comunicação com o DS18B20.
- `DallasTemperature` — leitura da temperatura.

As bibliotecas `Preferences`, `WiFi`, `WiFiClientSecure` e `HTTPClient` fazem parte do ecossistema Arduino-ESP32. As bibliotecas BLE podem variar conforme a implementação utilizada pelo pacote ESP32 instalado.

## Configuração do Supabase

O firmware utiliza a API do Supabase para reconhecer a coleira e enviar os dados coletados.

Antes de utilizar o dispositivo, configure no código:

- `SUPABASE_URL`: URL do projeto Supabase.
- `SUPABASE_KEY`: chave pública apropriada para acesso à API.

Não publique chaves secretas ou credenciais administrativas em repositórios públicos. Utilize somente uma chave apropriada para o firmware, com as permissões e políticas de segurança necessárias. Nunca coloque a chave `service_role` no dispositivo.

### Funções RPC esperadas

O firmware foi estruturado para utilizar funções do banco de dados com as seguintes responsabilidades:

| Função RPC | Responsabilidade |
|---|---|
| `reconhecer_coleira` | Validar a identidade da coleira |
| `registrar_localizacao_coleira` | Registrar localização e verificar áreas |
| `registrar_alerta_saida_area` | Registrar saída de uma área cadastrada |
| `registrar_temperatura_coleira` | Registrar a temperatura medida |
| `registrar_alerta_coleira` | Registrar um alerta relacionado à coleira |

Os nomes, parâmetros, tipos de retorno e políticas de acesso dessas funções devem corresponder exatamente aos definidos no banco de dados.

Parâmetros esperados pelo firmware:

- `reconhecer_coleira`: `p_num_serie`, `p_chave`
- `registrar_localizacao_coleira`: `p_num_serie`, `p_chave`, `p_latitude`, `p_longitude`
- `registrar_alerta_saida_area`: `p_num_serie`, `p_chave`, `p_id_area`, `p_distancia_metros`
- `registrar_temperatura_coleira`: `p_num_serie`, `p_chave`, `p_temperatura`
- `registrar_alerta_coleira`: `p_num_serie`, `p_chave`

**Importante:** confira os parâmetros e o formato de retorno das funções diretamente no Supabase. A função de localização, por exemplo, precisa retornar os dados de áreas no formato esperado pelo código que realiza a verificação geográfica.

## Identificação e segurança do dispositivo

Cada coleira utiliza dados próprios para sua identificação:

- Número de série individual.
- Chave individual de autenticação.
- Configurações de conexão Wi-Fi armazenadas localmente.

O número de série esperado pelo firmware possui 12 caracteres alfanuméricos em maiúsculas. A chave individual é representada por 32 caracteres hexadecimais minúsculos.

As credenciais são armazenadas na memória não volátil por meio de `Preferences`, permitindo que as configurações persistam após reinicializações e perda de alimentação.

Não reutilize a mesma chave entre dispositivos. O banco de dados deve validar a combinação entre o número de série e a chave, evitando que uma coleira se passe por outra.

## Configuração pelo aplicativo

O pareamento utiliza Bluetooth Low Energy (BLE) para transmitir as configurações necessárias à conexão Wi-Fi.

### UUIDs BLE

| Identificador | UUID |
|---|---|
| Serviço | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Configuração | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| Status | `beb5483e-36e1-4688-b7f5-ea07361b26a9` |
| ID do produto | `beb5483e-36e1-4688-b7f5-ea07361b26aa` |

### Formato de configuração

O aplicativo envia os dados à característica de configuração no seguinte formato:

```text
NUMERO_DE_SERIE
NOME_DA_REDE_WIFI
SENHA_DA_REDE_WIFI
```

Cada campo deve ser separado por uma quebra de linha (`\n`).

### Estados de pareamento

O firmware pode comunicar estados como:

| Estado | Significado |
|---|---|
| `PRONTA` | Aguardando configuração |
| `CONECTANDO` | Tentando conectar ao Wi-Fi |
| `OK` | Configuração ou conexão concluída |
| `RECONHECIDA` | Coleira validada pelo banco de dados |
| `RECUSADA` | Identificação recusada |
| `SEM_RESPOSTA` | Não houve resposta esperada |
| `ERRO_SERIAL` | Número de série inválido ou incompatível |
| `ERRO_WIFI` | Falha na conexão Wi-Fi |

Os estados exatos dependem da implementação atual do firmware e devem permanecer sincronizados com o aplicativo React Native.

## Monitor Serial

Após carregar o firmware, abra o Monitor Serial com a velocidade configurada no código.

O firmware disponibiliza comandos para configuração, diagnóstico e manutenção.

| Comando | Função |
|---|---|
| `setup` | Iniciar ou acessar a configuração |
| `pareamento` | Iniciar o pareamento BLE |
| `wifi` | Consultar ou configurar a conexão Wi-Fi |
| `redes` | Verificar redes Wi-Fi disponíveis |
| `status` | Consultar o estado atual do dispositivo |
| `mostrar` | Exibir informações armazenadas |
| `reconhecer` | Tentar reconhecer a coleira no Supabase |
| `somcalibrar` | Calibrar o sensor de som |
| `gps` | Consultar informações do GPS |
| `gpsraw` | Inspecionar dados brutos do GPS |
| `temp` | Consultar a temperatura |
| `tempmon` | Monitorar as leituras de temperatura |
| `reiniciar` | Reiniciar o ESP32-C3 |

A disponibilidade e a sintaxe exata dos comandos devem ser verificadas no código-fonte atual.

## Funcionamento do firmware

### 1. Inicialização

Ao iniciar, o ESP32-C3 carrega as configurações persistidas, inicializa os sensores e verifica a disponibilidade das conexões necessárias.

### 2. Configuração e pareamento

Se necessário, o usuário inicia o pareamento BLE e envia o número de série e as credenciais da rede Wi-Fi pelo aplicativo.

### 3. Conexão e reconhecimento

Após conectar-se à rede, o dispositivo realiza uma requisição ao Supabase para validar sua identificação.

### 4. Monitoramento da localização

O GPS fornece latitude e longitude. Quando há uma posição válida, o firmware pode enviá-la ao banco de dados para registro e verificação das áreas cadastradas.

### 5. Monitoramento da temperatura

O DS18B20 fornece as leituras de temperatura. Os valores são processados e enviados ao Supabase conforme os intervalos programados.

### 6. Detecção de sons

O sensor de som é monitorado para detectar sinais persistentes. Quando as condições programadas são satisfeitas, o firmware pode registrar um alerta.

### 7. Reconexão

Quando uma conexão falha, o firmware realiza novas tentativas conforme a lógica implementada. Os dados e o estado da conexão podem ser consultados pelo Monitor Serial.

## Intervalos de operação

Os intervalos configurados no código podem incluir:

| Operação | Intervalo |
|---|---|
| Verificação do GPS | 3 segundos |
| Envio da localização | 30 segundos |
| Leitura da temperatura | 5 segundos |
| Conversão da temperatura | 800 milissegundos |
| Envio da temperatura | 60 segundos |
| Tentativas de reconexão ou reconhecimento | 30 segundos |
| Reenvio de alerta de saída de área | 5 minutos |

Para o alerta de saída de área, o firmware utiliza mais de uma leitura fora da região antes de confirmar a ocorrência. A detecção de som também depende de um tempo mínimo de persistência.

Esses intervalos são referências da implementação e devem ser conferidos no código antes de qualquer alteração.

## Testes recomendados

### Teste 1 — Inicialização

1. Conecte o ESP32-C3 ao computador.
2. Abra o Monitor Serial.
3. Verifique se o firmware inicializa sem reinicializações inesperadas.
4. Execute `status`.

### Teste 2 — Sensores

1. Execute `gps` e verifique se o GPS recebe dados.
2. Utilize `gpsraw` para diagnosticar a comunicação serial.
3. Execute `temp` e confira a leitura do DS18B20.
4. Utilize `tempmon` para observar as leituras.
5. Utilize `somcalibrar` para ajustar a detecção do sensor de som.

### Teste 3 — Wi-Fi

1. Configure uma rede Wi-Fi de 2,4 GHz.
2. Verifique se a conexão foi estabelecida.
3. Execute `status`.
4. Caso falhe, confira a senha, o sinal e a compatibilidade da rede.

### Teste 4 — BLE

1. Inicie o pareamento pelo comando correspondente.
2. Abra o aplicativo.
3. Conecte-se ao serviço BLE da coleira.
4. Envie o número de série, o SSID e a senha.
5. Confira as atualizações de status.
6. Verifique se a coleira conecta ao Wi-Fi e é reconhecida pelo banco.

### Teste 5 — Supabase

1. Confira a URL e a chave utilizadas pelo firmware.
2. Verifique se as funções RPC existem.
3. Confira os parâmetros e os tipos de retorno.
4. Consulte os registros de execução e as políticas de segurança.
5. Confirme se os dados enviados aparecem nas tabelas esperadas.

## Limitações

- O GPS pode apresentar baixa precisão em ambientes fechados ou entre edifícios.
- O sensor de som detecta sinais acústicos conforme sua configuração, mas não identifica necessariamente latidos ou outros sons específicos.
- A leitura do DS18B20 representa a temperatura do próprio sensor; ela não garante uma medição precisa da temperatura corporal do animal.
- O funcionamento depende da disponibilidade de Wi-Fi e do acesso ao Supabase para as operações remotas.
- A autonomia depende da bateria, dos módulos utilizados e dos intervalos de operação.

O nome do arquivo `.ino` pode variar conforme o projeto.

## Licença
MIT
