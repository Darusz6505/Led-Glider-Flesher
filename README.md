
Oto profesjonalny, kompletny plik **`README.md`** gotowy do skopiowania i wklejenia bezpośrednio do Twojego repozytorium na GitHubie.

---

```markdown
# ✈️ Dariusz Fly Glider Flasher

[![ESP32-C3](https://img.shields.io/badge/Hardware-ESP32--C3%20SuperMini-brightgreen)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/Framework-Arduino-blue)](https://www.arduino.cc/)
[![IDE](https://img.shields.io/badge/IDE-PlatformIO%20%7C%20VS%20Code-orange)](https://platformio.org/)
[![BLE](https://img.shields.io/badge/BLE-NimBLE--Arduino%20v2.0%2B-navy)](https://github.com/h2zero/NimBLE-Arduino)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**Dariusz Fly Glider Flasher** to zaawansowany, mikroprocesorowy sterownik antykolizyjnego oświetlenia stroboskopowego LED przeznaczony dla szybowców i statków powietrznych. Urządzenie przetwarza cyfrowe depesze NMEA wysyłane przez systemy **FLARM** oraz **OGN Tracker**, wyzwalając adaptacyjne pakiety błysków ostrzegawczych zależne od poziomu zagrożenia kolizyjnego.

---

## 📋 Spis Treści
- [Główne Funkcje](#-główne-funkcje)
- [Architektura Systemu](#-architektura-systemu)
- [Specyfikacja Techniczna i Pinout](#-specyfikacja-techniczna-i-pinout)
- [Sekwencja Inicjalizacji (Booting)](#-sekwencja-inicjalizacji-booting)
- [Struktura Projektu i Partycje](#-struktura-projektu-i-partycje)
- [Instrukcja Kompilacji (PlatformIO)](#-instrukcja-kompilacji-platformio)
- [Panel WWW i Konfiguracja](#-panel-www-i-konfiguracja)
- [Bezpieczeństwo i Ochrona Termiczna](#-bezpieczeństwo-i-ochrona-termiczna)
- [Licencja](#-licencja)

---

## ✨ Główne Funkcje

* **Podwójny Odbiór NMEA:**
  * Przewodowe wejście **UART (RS-232 / TTL)** z regulowaną prędkością transmisji.
  * Bezprzewodowe łącze **Bluetooth Low Energy (BLE)** oparte na wydajnym stosie `NimBLE-Arduino v2.0+`.
  * Rozdzielone galwanicznie bufory pierścieniowe elimijące ryzyko mieszania ramek.
* **Algorytm RF Coexistence:**
  * Wstrzymanie punktu dostępowego Wi-Fi na czas nawiązywania połączenia BLE w celu oddania 100% mocy radia 2.4 GHz dla stabilnej negocjacji sygnału.
* **Hybrydowa Identyfikacja Urządzeń:**
  * Automatyczne rozpoznawanie trackerów OGN zarówno po **nazwie sieciowej** (np. `OGN604044`), jak i po sprzętowym **adresie MAC** (np. `64:E8:33:60:40:45`).
* **Skanowanie Awaryjne w Tle:**
  * Jeśli sygnał BLE zostanie utracony w locie, sterownik nieprzerwanie realizuje ochronę stroboskopową z portu UART, uruchamiając w tle asynchroniczny skaner co 30 sekund.
* **Konfiguracja przez Przeglądarkę (SoftAP):**
  * Dedykowany, responsywny interfejs WWW (`192.168.4.1`) z możliwością wyboru znalezionych urządzeń BLE z listy rozwijanej.
* **Bezprzewodowa Aktualizacja Firmware (OTA):**
  * Wbudowany serwer OTA umożliwiający wgrywanie nowych wersji oprogramowania (plików `.bin`) przez Wi-Fi bez demontażu urządzenia z panelu awioniki.
* **Zintegrowana Ochrona Termiczna:**
  * Ciągły monitor temperatury rdzenia ESP32-C3 automatycznie odłączający moduły radiowe w przypadku przegrzania.

---

## 🏗️ Architektura Systemu

```text
  +-----------------------+         +-------------------------+
  |    FLARM / RS-232     |         |   OGN Tracker (BLE)     |
  | (UART: RX=20, TX=21)  |         |  (Nordic NUS / HM-10)   |
  +-----------+-----------+         +------------+------------+
              |                                  |
              | [uartBuffer]                     | [bleBuffer]
              v                                  v
  +-----------------------------------------------------------+
  |                 ESP32-C3 SuperMini Core                   |
  |  - NMEA Parser ($PFLAU / $PFLAA)                          |
  |  - State Machine (IDLE / BLINK_ON / BLINK_OFF / DELAY)    |
  |  - Thermal & Power Management                             |
  +-----------------------------+-----------------------------+
                                |
                                v
                   +-------------------------+
                   | High-Power MOSFET Gate  |
                   |      (GPIO 1 / A3)      |
                   +------------+------------+
                                |
                                v
                   +-------------------------+
                   | Stroboskop LED Szybowca |
                   +-------------------------+

```

---

## 🔌 Specyfikacja Techniczna i Pinout

| Komponent / Funkcja | Pin GPIO ESP32-C3 | Opis / Uwagi |
| --- | --- | --- |
| **Wyjście MOSFET Stroboskopu** | `GPIO 1` | Sterowanie bramką N-MOSFET (Sygnał HIGH = WŁ) |
| **Dioda LED Statusu (Board)** | `GPIO 8` | Wbudowana dioda LED (Sygnał LOW = WŁ) |
| **FLARM UART RX** | `GPIO 20` | Odbiór danych NMEA z portu szeregowego |
| **FLARM UART TX** | `GPIO 21` | Nadawanie (opcjonalne) |
| **Zasilanie Układu** | `VCC / GND` | 5V DC (zintegrowany stabilizator LDO) |

---

## 🔄 Sekwencja Inicjalizacji (Booting)

Sterownik realizuje przewidywalny, sekwencyjny proces uruchamiania:

1. **Sprzętowy Test LED:** Wykonanie **4 twardych błysków** stroboskopu w pierwszej milisekundzie po podłączeniu zasilania.
2. **Odczyt Pamięci Flash (NVM):** Wczytanie zapisanych preferencji (SSID, Hasło, Baud rate, ID Trackera).
3. **Dedykowane Skanowanie BLE (15 sekund):** Zapewnienie pełnej mocy radia dla NimBLE (Wi-Fi wyłączone), przeszukanie eteru i wygenerowanie listy urządzeń.
4. **Zestawienie Połączenia:** Automatyczne połączenie ze skonsolidowanym trackerem OGN.
5. **Aktywacja Wi-Fi i WWW:** Uruchomienie Access Pointa oraz panelu konfiguracyjnego `http://192.168.4.1`.

---

## 📁 Struktura Projektu i Partycje

Projekt przygotowany jest dla środowiska **PlatformIO**:

```text
Dariusz-Fly-Glider-Flasher/
 ├── platformio.ini         # Konfiguracja środowiska i zależności
 ├── custom_ota.csv         # Zoptymalizowana tabela partycji (Dual OTA)
 └── src/
      └── main.cpp          # Główny kod źródłowy w języku C++

```

### Schemat Partycji (`custom_ota.csv`)

Zastosowano zoptymalizowany podział pamięci Flash (4 MB) z dedykowanymi slotami na aktualizacje bezprzewodowe OTA oraz rozbudowane biblioteki radiowe:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1E0000,
app1,     app,  ota_1,   0x1F0000,0x1E0000,
spiffs,   data, spiffs,  0x3D0000,0x20000,
coredump, data, coredump,0x3F0000,0x10000,

```

---

## 🛠️ Instrukcja Kompilacji (PlatformIO)

### Wymagania

1. [Visual Studio Code](https://code.visualstudio.com/) z zainstalowaną wtyczką **PlatformIO IDE**.
2. Zainstalowany sterownik USB-UART dla ESP32-C3.

### Krok po kroku

1. Sklonuj repozytorium:
```bash
git clone [https://github.com/twoj-login/Dariusz-Fly-Glider-Flasher.git](https://github.com/twoj-login/Dariusz-Fly-Glider-Flasher.git)

```


2. Otwórz folder projektu w **VS Code**.
3. Zawartość pliku `platformio.ini`:
```ini
[env:esp32-c3-devkitm-1]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino

monitor_speed = 115200
board_build.partitions = custom_ota.csv

lib_deps =
    h2zero/NimBLE-Arduino @ ^2.0.0

```


4. Kliknij ikonę **Build** (✓) lub użyj skrótu `Ctrl+Alt+B`.
5. Podłącz płytkę ESP32-C3 SuperMini i kliknij **Upload** (➔).

---

## 🌐 Panel WWW i Konfiguracja

Podłącz się do sieci Wi-Fi wygenerowanej przez sterownik i otwórz przeglądarkę pod adresem `http://192.168.4.1`.

* **Domyślny SSID:** `FLARM_Strobe_XXX` (losowy przyrostek)
* **Domyślne Hasło:** `szybowiec`

### Dostępne Opcje w Panelu:

* **Ochrona Termiczna:** Ustawianie górnego limitu wyłączenia (60–80°C) oraz dolnego limitu ponownego załączenia (50–70°C).
* **Sieć Wi-Fi:** Zmiana nazwy SSID, hasła oraz czasowego wyłącznika AP (5 min, 10 min, praca ciągła).
* **Port Szeregowy:** Wybór prędkości UART (`4800`, `19200`, `38400`, `115200` baud).
* **OGN Tracker BLE:** Wybór urządzenia z listy rozwijanej znalezionych w eterze lub ręczne wpisanie adresu MAC/Nazwy.
* **Ustawienia Błysków:** Wybór liczby błysków w paczce (2–4), zmiana interwału błysków prewencyjnych (5–30 s / wyłączone) oraz wyłącznik błysków na postoju.
* **Podgląd NMEA Live:** Odbiór i wyświetlanie ramek $PFLAU / $PFLAA w czasie rzeczywistym.
* **Aktualizacja OTA:** Przejście do podstrony `/update` pozwalającej na wgranie nowej kompilacji oprogramowania.

---

## 🌡️ Bezpieczeństwo i Ochrona Termiczna

Ze względu na ograniczoną przestrzeń w panelu awioniki i pracę przy wysokich temperaturach w okresie letnim, sterownik wyposażono w aktywny monitoring rdzenia ESP32:

* **Stan Przekroczenia (`temp > tempUpperLimit`):** Wyłączenie modułów Wi-Fi i Bluetooth, odłączenie połączenia BLE, przejście w tryb czysto sprzętowej obsługi portu UART.
* **Stan Schłodzenia (`temp <= tempLowerLimit`):** Automatyczne przywrócenie pracy modułów bezprzewodowych oraz wznowienie połączenia BLE.

---

## 📜 Licencja

Projekt udostępniany jest na licencji **MIT**. Szczegółowe informacje znajdują się w pliku `LICENSE`.

---

*Projekt stworzony z myślą o zwiększeniu bezpieczeństwa w lotnictwie grawitacyjnym.*

```

```
