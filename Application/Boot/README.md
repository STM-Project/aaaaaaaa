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
