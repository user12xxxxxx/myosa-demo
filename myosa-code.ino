#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "oled.h" 
#include "LightProximityAndGesture.h" 
#include "AccelAndGyro.h"             

// ==========================================
// --- User Configuration & Credentials ---
// ==========================================
const char* ssid = "ninja-H2";         
const char* password = "n2002das"; 
const char* mqtt_server = "10.175.74.41"; 
const char* mqtt_user = "Navalogy";     
const char* mqtt_pass = "Man_Mera_Mandir";     

// ==========================================
// --- Global Object Instantiation ---
// ==========================================
oLed display(128, 64);                
WiFiClient espClient;                 
PubSubClient client(espClient);       
LightProximityAndGesture apds;        
AccelAndGyro mpu;                     

// ==========================================
// --- Hardware Pin Definitions ---
// ==========================================
#define DAC_PIN 25                    
#define ADC_PIN 32                    
#define ENB 23                        
#define IN3 26                        
#define IN4 27                        
const int LEDs[] = {5, 18, 19};       

// ==========================================
// --- State & Buffer Variables ---
// ==========================================
uint16_t pool[21] = {0};              

// Separate Timers
unsigned long lastGenSync = 0;        
unsigned long lastDisplaySync = 0;    
unsigned long lastMacSync = 0;        
unsigned long lastMotor = 0;          
unsigned long lastLED = 0;            
unsigned long lastAdcSync = 0;        // Timer for 10ms ADC/DAC polling

// Timing Constants
const long genInterval = 100;         
const long displayInterval = 250;     
const long macInterval = 5000;        

int ledIdx = 0;                       
bool motorDir = true;                 

// MYOSA Staggered Reading Variables
unsigned long previousSensorMillis = 0; 
const long perModuleInterval = 100; 
uint8_t nScreen = 0u;

// ADC Zero Streak Counter
int zeroCount = 0;

String csvBuffer = "";                
String paramsBuffer = "";             
char formattedNum[5] = "0000";        

// ==========================================
// --- GLOBAL SENSOR VARIABLES ---
// ==========================================
float t = 0, ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
uint16_t red = 0, grn = 0, blu = 0, amb = 0;
uint16_t pm1 = 0, pm25 = 0, pm10 = 0;
uint16_t p03 = 0, p05 = 0, p10 = 0, p25 = 0, p50 = 0, p100 = 0;
uint16_t mosfetNoise = 0;

// ==========================================
// --- Helper Functions ---
// ==========================================
uint16_t readAPDSColor(uint8_t reg) {
  Wire.beginTransmission(0x39);       
  Wire.write(reg);                    
  if (Wire.endTransmission(false) != 0) return 0; 
  Wire.requestFrom(0x39, 2);          
  if (Wire.available() >= 2) return Wire.read() | (Wire.read() << 8); 
  return 0;                           
}

// ==========================================
// --- Initial Setup Routine ---
// ==========================================
void setup() {
  Serial.begin(115200);               
  Serial2.begin(9600, SERIAL_8N1, 17, 16); 
  
  Wire.begin(21, 22);
  Wire.setClock(100000); 

  if(display.begin()) {
    display.clearDisplay();
    display.setTextSize(2); 
    display.setCursor(20, 15);
    display.print("Booting");
    display.display();
  }
  delay(500); 

  mpu.begin(); 
  apds.begin(); 

  // Force APDS9960 Color/ALS Engine to Wake Up
  Wire.beginTransmission(0x39);
  Wire.write(0x80); 
  Wire.write(0x03); 
  Wire.endTransmission();

  WiFi.begin(ssid, password);
  client.setServer(mqtt_server, 1883); 
  
  pinMode(ENB, OUTPUT); 
  pinMode(IN3, OUTPUT); 
  pinMode(IN4, OUTPUT);
  for(int i = 0; i < 3; i++) pinMode(LEDs[i], OUTPUT);         
}

// ==========================================
// --- Main Execution Loop ---
// ==========================================
void loop() {
  unsigned long currentMillis = millis();       
  
  if (!client.connected()) reconnect();
  client.loop();                      

  // ----------------------------------------
  // 1. Actuator Control
  // ----------------------------------------
  if (currentMillis - lastLED >= 150) {
    for(int i = 0; i < 3; i++) digitalWrite(LEDs[i], LOW); 
    digitalWrite(LEDs[ledIdx], HIGH);                      
    ledIdx = (ledIdx + 1) % 3;                             
    lastLED = currentMillis;
  }
  
  if (currentMillis - lastMotor >= 1500) {
    motorDir = !motorDir;             
    digitalWrite(IN3, motorDir);      
    digitalWrite(IN4, !motorDir);
    analogWrite(ENB, 255);            
    lastMotor = currentMillis;
  }

  // ----------------------------------------
  // 2. Data Harvesting (MYOSA Staggered Method)
  // ----------------------------------------
  if (currentMillis - previousSensorMillis >= perModuleInterval) {
    previousSensorMillis = currentMillis;

    switch (nScreen) {
      case 0u: 
        // Pulling temperature directly from the MPU6050
        t = mpu.getTempC(false);
        nScreen = 1u;
        break;
        
      case 1u: 
        ax = mpu.getAccelX(false);
        ay = mpu.getAccelY(false);
        az = mpu.getAccelZ(false);
        nScreen = 2u;
        break;
        
      case 2u: 
        gx = mpu.getGyroX(false);
        gy = mpu.getGyroY(false);
        gz = mpu.getGyroZ(false);
        nScreen = 3u;
        break;
        
      case 3u: 
        red = readAPDSColor(0x96);
        grn = readAPDSColor(0x98);
        blu = readAPDSColor(0x9A);
        amb = readAPDSColor(0x94);
        nScreen = 4u;
        break;

      case 4u: 
        if (Serial2.available() >= 32) {
          if (Serial2.peek() == 0x42) {
            uint8_t buf[32];
            Serial2.readBytes(buf, 32);       
            if (buf[1] == 0x4D) { 
              pm1  = (buf[4] << 8) | buf[5];
              pm25 = (buf[6] << 8) | buf[7];
              pm10 = (buf[8] << 8) | buf[9];
              p03  = (buf[16] << 8) | buf[17];
              p05  = (buf[18] << 8) | buf[19];
              p10  = (buf[20] << 8) | buf[21];
              p25  = (buf[22] << 8) | buf[23];
              p50  = (buf[24] << 8) | buf[25];
              p100 = (buf[26] << 8) | buf[27];
            }
          } else {
            Serial2.read(); 
          }
        }
        nScreen = 0u; 
        break;

      default:
        nScreen = 0u;
        break;
    }
  }

  // Populate pool
  pool[0] = mosfetNoise; pool[1] = (uint16_t)t;   pool[2] = (uint16_t)ax; 
  pool[3] = (uint16_t)ay; pool[4] = (uint16_t)az;  pool[5] = (uint16_t)gx; 
  pool[6] = (uint16_t)gy; pool[7] = (uint16_t)gz;  pool[8] = red; 
  pool[9] = grn;         pool[10] = blu;         pool[11] = amb; 
  pool[12] = pm1;        pool[13] = pm25;        pool[14] = pm10;
  pool[15] = p03;        pool[16] = p05;         pool[17] = p10;
  pool[18] = p25;        pool[19] = p50;         pool[20] = p100;

  // ----------------------------------------
  // 3. Hardware Entropy & DAC (10ms Loop)
  // ----------------------------------------
  if (currentMillis - lastAdcSync >= 10) {
    // Drive DAC with pure hardware random noise
    dacWrite(DAC_PIN, esp_random() & 0xFF);
    
    // Seed the Arduino PRNG with hardware true random to ensure high-frequency extraction stays chaotic
    randomSeed(esp_random()); 
    
    uint16_t adcValue = analogRead(ADC_PIN);
    
    if (adcValue == 0) {
      zeroCount++;
    } else {
      if (zeroCount > 0) {
        mosfetNoise = zeroCount; // Output the zero streak
        zeroCount = 0;           // Reset streak
      } else {
        mosfetNoise = adcValue;  // Output the non-zero reading
      }
    }
    
    lastAdcSync = currentMillis;
  }

  // ----------------------------------------
  // 4. Data Generation & MQTT Publish (HIGH FREQUENCY)
  // ----------------------------------------
  if (currentMillis - lastGenSync >= genInterval) {
    
    uint8_t digit_array[16];
    for (int i = 0; i < 16; i++) {
      int p_idx = random(0, 21);     
      uint16_t val = pool[p_idx];
      
      // Zero-Fix: Don't extract from a 0, just inject a random 1-9
      if (val == 0) {
        digit_array[i] = random(1, 10);
        continue;
      }
      
      // Dynamic Length Fix: Only extract valid digits
      int max_digits = 1;
      if (val > 9) max_digits = 2;
      if (val > 99) max_digits = 3;
      if (val > 999) max_digits = 4;
      if (val > 9999) max_digits = 5;
      
      int digit_pos = random(0, max_digits);     
      int divisor = 1;
      for (int j = 0; j < digit_pos; j++) divisor *= 10;
      
      digit_array[i] = (val / divisor) % 10; 
    }

    for (int i = 0; i < 4; i++) {
      uint32_t gen_num = 0;
      for (int j = 0; j < 4; j++) {
        int rand_idx = random(0, 16);
        uint8_t d = digit_array[rand_idx];
        
        // Zero-Fix: Force the very first digit to be 1-9
        if (j == 0 && d == 0) {
          d = random(1, 10);
        }
        
        gen_num = (gen_num * 10) + d; 
      }

      if (gen_num > 0) { 
        char tempNum[5];
        sprintf(tempNum, "%04lu", gen_num); 
        
        if (csvBuffer != "") csvBuffer += ",";
        csvBuffer += String(tempNum);
        
        // Store the latest number for the OLED
        strcpy(formattedNum, tempNum); 
      }
    }

    // --- MAC Address Injection ---
    if (currentMillis - lastMacSync >= macInterval) {
      if (csvBuffer != "") csvBuffer += ",";
      csvBuffer += "MAC:" + WiFi.macAddress();
      lastMacSync = currentMillis;
    }

    paramsBuffer = String(mosfetNoise) + "," + String(t) + "," + String(ax) + "," + String(ay) + "," + String(az) + "," +
                   String(gx) + "," + String(gy) + "," + String(gz) + "," + String(red) + "," +
                   String(grn) + "," + String(blu) + "," + String(amb) + "," +
                   String(pm1) + "," + String(pm25) + "," + String(pm10) + "," +
                   String(p03) + "," + String(p05) + "," + String(p10) + "," + String(p25) + "," + String(p50) + "," + String(p100);

    if (client.connected()) {
      client.publish("random/numbers", csvBuffer.c_str());
      client.publish("random/params", paramsBuffer.c_str());
    }
    
    csvBuffer = "";
    lastGenSync = currentMillis;
  }

  // ----------------------------------------
  // 5. OLED Display Update (LOW FREQUENCY)
  // ----------------------------------------
  if (currentMillis - lastDisplaySync >= displayInterval) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(10, 0);
    display.print("TU-ANKAJA");          
    
    display.setTextSize(3);
    display.setCursor(28, 30);
    display.print(formattedNum);      
    display.display();

    lastDisplaySync = currentMillis;
  }
}

// ==========================================
// --- MQTT Reconnection Routine ---
// ==========================================
void reconnect() {
  while (!client.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(ssid, password);
        delay(2000); 
    }
    
    if (client.connect("Ankaja_Node", mqtt_user, mqtt_pass)) {
      Serial.println("MQTT Connected");
    } else {
      delay(500);                     
    }
  }
}
