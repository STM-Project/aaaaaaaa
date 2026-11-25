# Bootloader STM32H753 z ekranem aktualizacji

Projekt `stm32h753-boot` buduj przez zwykłe **Build Project** w
STM32CubeIDE lub przez `python3 tools/build_lcd.py`. Wgraj wynikowy HEX
przez ST-LINK. Tablica wektorów bootloadera jest od `0x08000000`, a obraz
aplikacji musi od `0x08020000`. Po zwykłym resecie i przy prawidłowym
obrazie bootloader przekazuje sterowanie bez inicjalizacji LCD i ESP32.

W działającej aplikacji H7 wybierz profil STA w `/wifi`, wejdź na
`/upload` lub `/ulpad` i podaj klucz aktualizacji. Bootloader użyje
przekazanego profilu sieci. Jeżeli brak poprawnej aplikacji, połącz się
z siecią `STM32H753-Boot` i otwórz `http://192.168.4.1/upload`. Hasło
tej sieci jest losowane przy starcie i wyświetlane na LCD.

Przeglądarka pokazuje postęp przesyłania pliku. LCD pokazuje postęp
odbioru i zapisu we Flash, weryfikację CRC oraz wynik. Po udanym
zapisie STM32 wykonuje reset i uruchamia aplikację.

USART6 przełącza ESP32 z fabrycznej prędkości 115200 na 2000000 bodów
(8N1), tak jak aplikacja H7. Jeśli moduł odpowiada już przy 2000000,
bootloader pozostaje na tej prędkości. Odbiór HTTP pozostaje pasywny
(`AT+CIPRECVMODE=1`), dzięki czemu STM32 może wstrzymać pobieranie danych
na czas kasowania i programowania Flash.

Log bootloadera jest na tym samym wyjściu co log aplikacji głównej.
Ustaw terminal na UART7 TX (PF7), 2000000 bodów, 8N1. Linie `[BOOT]`
pokazują przejście do aplikacji, próbę połączenia przez ESP32, adres IP
i port serwera HTTP.

## Debug w STM32CubeIDE

Zbuduj konfigurację **Debug**, a następnie uruchom dołączony profil
`STM32h753bit6 Debug.launch` w projekcie bootloadera. Profil wgrywa plik
ELF z symbolami i zatrzymuje procesor na początku `Boot_Main`. Użyj
**F5** (Step Into) lub **F6** (Step Over). Jeżeli IDE ma zapisaną starszą
konfigurację, wybierz dołączony plik `.launch` z tego projektu.

Aplikacja główna i bootloader mają tę samą nazwę projektu Eclipse.
Jeśli otwierasz oba w CubeIDE, użyj dwóch workspace'ów i wybierz profil
Debug w workspace bootloadera.
