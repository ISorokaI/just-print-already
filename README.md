# Just Print Already (JPA)

Приложение для macOS 27 на Apple Silicon, которое заново подключает старые USB-принтеры. Ни Rosetta, ни драйвер HP, ни Ghostscript не нужны.

*A macOS app that sets up old host-based USB printers on Apple Silicon with native arm64 CUPS filters.*

## Зачем

Дешёвые лазерники вроде HP LaserJet M1132, P1102 и 1022 сами страницу не рисуют: компьютер присылает им готовую картинку в закрытом формате ZjStream. Драйверы HP для них собраны только под Intel. CUPS в macOS 27 такие фильтры не запускает даже при установленной Rosetta: задание падает с `com.apple.badarch-error`. AirPrint у этих принтеров нет.

JPA находит принтер на USB, подбирает нативный arm64-фильтр и создаёт очередь печати. Пользователь нажимает одну кнопку и вводит пароль администратора.

## Поддерживаемые принтеры

| Принтер | Проверен |
|---|---|
| HP LaserJet Professional M1132 / M1134 / M1136 / M1137 / M1138 / M1139 MFP | ✅ ([hp-m1132-arm64](https://github.com/Diego-Rebello/hp-m1132-arm64)) |
| HP LaserJet Professional P1102 / P1102w, P1566, P1606dn | — |
| HP LaserJet 1022 / 1022n / 1022nw, M1319 MFP, P2035 | — |
| Minolta magicolor 2200 DL / 2300 DL | — |

Список задаётся в [`drivers/models.tsv`](drivers/models.tsv). Если у вас есть принтер из списка, откройте issue с результатом.

Пока не поддерживаются HP LaserJet 1000/1005/1018/1020: им при каждом включении нужно загружать прошивку.

## Сборка

Нужны Xcode Command Line Tools.

```sh
make test   # юнит-тесты
make app    # build/JustPrintAlready.app
make dmg    # build/JustPrintAlready.dmg
```

## Что делает кнопка «Настроить»

1. Генерирует PPD из [`drivers/zjs/template.ppd`](drivers/zjs/template.ppd) под модель принтера.
2. С правами администратора копирует фильтр в `/Library/Printers/JustPrintAlready/`. В `/usr/libexec/cups` ничего не пишется.
3. Создаёт очередь `JPA_<модель>` через `lpadmin`, без общего доступа.

Удаление:

```sh
sudo lpadmin -x JPA_<модель>
sudo rm -rf /Library/Printers/JustPrintAlready
```

## Лицензия

GPL-2.0-or-later. Фильтры основаны на foo2zjs (Rick Richardson) через [printer-all](https://github.com/faradayfury/printer-all) и [hp-m1132-arm64](https://github.com/Diego-Rebello/hp-m1132-arm64). Подробнее в [NOTICE.md](NOTICE.md).
