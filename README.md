# STM32H753_BOOT_ESP

Samodzielny bootloader dla płyty STM32H753BIT6 z projektu STM-Project/stm32h753.
Zawiera kod startowy H753 oraz potrzebne pliki HAL i CMSIS. Łączy się z ESP32
przez USART6 na PC6/PC7 z zamienionymi liniami TX/RX tak jak aplikacja główna.
Sygnał EN modułu ESP32 jest na PA9. Do uruchomienia potrzebny jest ESP-AT
obsługujący pasywny odbiór TCP (`AT+CIPRECVMODE=1`, `AT+CIPRECVLEN?`,
`AT+CIPRECVDATA`). Obsługiwane są początkowe prędkości 115200 i 2000000 bodów.

## Budowanie i pierwsze wgranie

1. Zainstaluj GNU Arm Embedded Toolchain dla Cortex-M7 z Newlib Nano, GNU Make
   oraz Python 3. Dodaj narzędzia `arm-none-eabi-*` do `PATH`.
2. W katalogu projektu wykonaj `make -j4`. Projekt można także otworzyć w
   STM32CubeIDE jako istniejący projekt Makefile, wskazując ten katalog.
3. Przy pierwszej kompilacji generator zapisze losowe hasło do lokalnego pliku
   `wifi-password`. Zachowaj je. Aby ustawić własne, zapisz przed kompilacją
   od 12 do 63 znaków z zakresu liter, cyfr, myślnika i podkreślenia.
4. W STM32CubeProgrammer wgraj przez ST-LINK plik `build/bootloader.hex`.
   Dla surowego `build/bootloader.bin` podaj adres `0x08000000`.
   Projekt zakłada domyślną kolejność banków Flash i start wewnętrznego Flash.
   Jeśli na płycie istnieje aplikacja pod `0x08000000`, zastąp ją wariantem
   relokowanym opisanym poniżej.

## Wgrywanie aplikacji H7 przez Wi-Fi

1. W repozytorium aplikacji H7 wykonaj
   `python3 tools/build_lcd.py --bootloader-app`. Do aktualizacji wybierz
   `Debug/bootloader-app/firmware.bin` i zachowaj lokalny plik `update-key`
   z tego samego katalogu.
2. Gdy poprawna aplikacja jest już w pamięci, zwykły reset uruchamia ją od
   razu. W `/wifi` ustaw profil STA i poczekaj, aż ESP32 dołączy do tej sieci.
   Z komputera w tej samej sieci otwórz `/upload` lub `/ulpad` pod adresem
   interfejsu STA i podaj klucz aktualizacji. Aplikacja przekazuje profil
   i klucz do bootloadera przez SRAM podtrzymywaną i wykonuje reset.
3. ESP32 ponownie dołącza w trybie STA do wskazanej sieci z tym samym portem
   HTTP i ustawieniami DHCP/statycznego adresu. Przeglądarka próbuje otworzyć
   stronę bootloadera pod dotychczasowym adresem. Jeżeli DHCP zmieni adres,
   odszukaj urządzenie `Elektronika_STM` w routerze. W ciągu dwóch minut
   wybierz BIN i kliknij **Wgraj i uruchom**. Jeśli przeglądarka nie przeniosła
   klucza między stronami, wpisz go ponownie.
4. Po sprawdzeniu zapisu bootloader wyłącza ESP32, przekazuje aplikacji MSP
   i VTOR oraz skacze do jej Reset_Handler.

Przy pustej pamięci lub uszkodzonej aplikacji bootloader uruchamia sieć
serwisową `STM32H753-Boot`. Połącz się z hasłem z pliku `wifi-password` i
otwórz `http://192.168.4.1/upload`. W tej sieci klucz aktualizacji zostaw
pusty. W trybie STA używaj zaufanej sieci lokalnej, ponieważ HTTP nie
szyfruje klucza. Jeżeli połączenie z routerem zawiedzie, poprawna aplikacja
jest uruchamiana ponownie.

Przepływ „przeglądarka → ESP-AT → STM32” i ścieżkę `/upload` przeniesiono
z projektu F103. Strona H7 przesyła surowy BIN do
`POST /uploader.php?crc=<8 cyfr szesnastkowych>`, z `Content-Length` i
`Content-Type: application/octet-stream`. Używaj strony dostarczonej
przez ten bootloader. Format F103 oparty na pakietach Base64 i MD5 różni
się od protokołu H7.

| Obszar | Zakres |
| --- | --- |
| Bootloader | `0x08000000–0x0801FFFF` |
| Aplikacja | `0x08020000–0x081FFFFF` |

Bootloader sprawdza długość, adres startu, MSP, CRC32 oraz odczyt po każdym
zapisie. Pierwszy 32-bajtowy blok z wektorami zapisuje jako ostatni. Po
przerwanym wgrywaniu ponów aktualizację. Układ ma jeden obszar aplikacji,
więc po skasowaniu jej sektora nie można wrócić do poprzedniego obrazu bez
ponownego wgrania. Dostęp do aktualizacji jest ograniczony kluczem w trybie STA lub hasłem
WPA2 w sieci serwisowej. CRC32 wykrywa błędy transmisji, a projekt nie weryfikuje podpisu
firmware.

## Testy

`make check` uruchamia testy ESP-AT, fragmentacji HTTP, programowania
przez granicę banków, CRC i przerwanego transferu na hoście.
Pierwszy test na płytce wykonaj z dostępem do ST-LINK. Sprawdź start AP,
wgranie BIN, skok i obsługę przerwań przez aplikację.
