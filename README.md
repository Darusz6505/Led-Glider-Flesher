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
🔌 Specyfikacja Techniczna i PinoutKomponent / FunkcjaPin GPIO ESP32-C3Opis / UwagiWyjście MOSFET StroboskopuGPIO 1Sterowanie bramką N-MOSFET (Sygnał HIGH = WŁ)Dioda LED Statusu (Board)GPIO 8Wbudowana dioda LED (Sygnał LOW = WŁ)FLARM UART RXGPIO 20Odbiór danych NMEA z portu szeregowegoFLARM UART TXGPIO 21Nadawanie (opcjonalne)Zasilanie UkładuVCC / GND5V DC (zintegrowany stabilizator LDO)🔄 Sekwencja Inicjalizacji (Booting)Sterownik realizuje przewidywalny, sekwencyjny proces uruchamiania:Sprzętowy Test LED: Wykonanie 4 twardych błysków stroboskopu w pierwszej milisekundzie po podłączeniu zasilania.Odczyt Pamięci Flash (NVM): Wczytanie zapisanych preferencji (SSID, Hasło, Baud rate, ID Trackera).Dedykowane Skanowanie BLE (15 sekund): Zapewnienie pełnej mocy radia dla NimBLE (Wi-Fi wyłączone), przeszukanie eteru i wygenerowanie listy urządzeń.Zestawienie Połączenia: Automatyczne połączenie ze skonsolidowanym trackerem OGN.Aktywacja Wi-Fi i WWW: Uruchomienie Access Pointa oraz panelu konfiguracyjnego http://192.168.4.1.📁 Struktura Projektu i PartycjeProjekt przygotowany jest dla środowiska PlatformIO:Plaintext

