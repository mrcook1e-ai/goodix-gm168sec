# goodix-gm168sec

**[English](README.md) | [Русский](README.ru.md)**

Драйвер для сканера отпечатков **Goodix GM168SEC** (`27c6:589a`) в Linux.
Работает через `libfprint` и `fprintd`, использует OpenSSL 3 и SIGFM.

[![CI](https://github.com/mrcook1e-ai/goodix-gm168sec/actions/workflows/ci.yml/badge.svg)](https://github.com/mrcook1e-ai/goodix-gm168sec/actions/workflows/ci.yml)
[![Release](https://img.shields.io/badge/Fedora_45-RPM-51a2da?logo=fedora&logoColor=white)](https://github.com/mrcook1e-ai/goodix-gm168sec/releases)
[![License](https://img.shields.io/badge/license-LGPL--2.1--or--later-blue)](LICENSE)

> Проект экспериментальный. Подтверждён только сканер `27c6:589a` с прошивкой
> `GFUSB_GM168SEC_APP_10036`. Не отключайте вход по паролю.

## Установка на Fedora 45 (x86_64)

Собирать ничего не нужно:

1. Скачайте файл **`libfprint-goodix-gm168-0.2.0-...fc45.x86_64.rpm`** со
   [страницы релизов](https://github.com/mrcook1e-ai/goodix-gm168sec/releases).
2. Установите его:

   ```bash
   sudo dnf install ./libfprint-goodix-gm168-0.2.0-*.fc45.x86_64.rpm
   ```

3. Переподключите сканер, если он внешний. Затем добавьте палец через настройки
   системы или командой:

   ```bash
   fprintd-enroll -f right-thumb
   ```

4. Проверьте распознавание:

   ```bash
   fprintd-verify -f right-thumb
   ```

RPM сам устанавливает правило udev и настройку `fprintd`. Если `fprintd` уже был
запущен, установщик перезапустит его.

### Что важно знать

- Системный `libfprint` **не заменяется**. Драйвер лежит отдельно в
  `/opt/libfprint-gm168`, а настройка применяется только к `fprintd.service`.
- Эта сборка содержит драйвер **только для GM168**. Пока установлен пакет,
  `fprintd` не должен использоваться с другими моделями сканеров.
- Старые шаблоны NBIS несовместимы с SIGFM. Если палец был добавлен старой
  версией драйвера, удалите шаблон и зарегистрируйте его заново:

  ```bash
  fprintd-delete "$USER"
  fprintd-enroll -f right-thumb
  ```

## PSK и Windows

Если `/etc/goodix-gm168/psk.bin` отсутствует, драйвер пытается прочитать уже
существующий ключ сканера и использует его только в памяти. Отдельная настройка
Windows или ручное восстановление ключа обычно не нужны.

Повторная инициализация сканера в Windows может сменить ключ. Свой `psk.bin`,
если он есть, храните приватно; не отправляйте его в issues и не публикуйте вместе
с логами, калибровками или изображениями отпечатков.

Подробности: [PSK recovery](docs/PSK-RECOVERY.md) и [FAQ](docs/FAQ.md).

## Удаление

```bash
sudo dnf remove libfprint-goodix-gm168
```

После удаления `fprintd` снова использует системный `libfprint`. Файлы в
`/etc/goodix-gm168` и сохранённые шаблоны отпечатков пакет не удаляет.

## Текущее состояние

На одном устройстве после полного сброса шаблонов текущая сборка прошла живую
проверку: регистрация `right-thumb` завершилась на `20/20` этапов, все пять
сессий проверки завершились совпадением — четыре с первого касания и одна после
повторного касания. TLS- и watchdog-ошибок в этой короткой серии не было.

Это результат на **одном устройстве и малом числе касаний**, а не измерение
FAR/FRR и не гарантия для других сканеров. Другие прошивки, suspend/resume и
массовая совместимость пока не проверены.

## Сборка из исходников и технические детали

Пользователю Fedora достаточно RPM выше. Если хотите разобраться или помочь с
разработкой:

- [INSTALL.md](INSTALL.md) — зависимости, сборка и локальная сборка RPM;
- [docs/README.md](docs/README.md) — все технические материалы;
- [Архитектура](docs/ARCHITECTURE.md) и [state machines](docs/STATE-MACHINES.md);
- [USB/TLS protocol](docs/PROTOCOL.md);
- [Image pipeline](docs/IMAGE-PIPELINE.md);
- [SIGFM matching](docs/SIGFM.md);
- [Тестирование](docs/TESTING.md) и [разработка](docs/DEVELOPMENT.md).

Правила участия: [CONTRIBUTING.md](CONTRIBUTING.md). Сообщения об ошибках:
[Issues](https://github.com/mrcook1e-ai/goodix-gm168sec/issues). Не прикладывайте
ключи, шаблоны, калибровки, снимки отпечатков и USB-дампы.

## Credits

Спасибо всем, чьи исследования, код и проверки сделали этот проект возможным:

| Участник / проект | Вклад |
|---|---|
| **Oleg ([@mrcook1e-ai](https://github.com/mrcook1e-ai))** | Исходное исследование GM168, USB-протокол, TLS-PSK, асинхронные state machines и image pipeline. [`goodix-gm168`](https://github.com/mrcook1e-ai/goodix-gm168) |
| **Daniil Yakoshinov ([@yakoshiq](https://github.com/yakoshiq))** | Проверка на оборудовании, безопасность и lifecycle, структура overlay и упаковка. [`libfprint-goodix-gm168`](https://github.com/yakoshiq/libfprint-goodix-gm168) |
| **wseng ([@wseng](https://github.com/wseng))** | Ранняя реализация драйвера и вклад в исследование протокола. |
| **Space-Dev0 ([@Space-Dev0](https://github.com/Space-Dev0))** | Драйвер Goodix `27c6:581a`, reciprocal matching, tiled capture-coverage gate и deferred deactivation. [`goodix-fingerprint-drivers`](https://github.com/Space-Dev0/goodix-fingerprint-drivers) |
| **Alexander Meiler ([@TheWeirdDev](https://github.com/TheWeirdDev))**, **Matthieu Charette ([@mpi3d](https://github.com/mpi3d))**, **Alireza S.N. ([@alirezasn80](https://github.com/alirezasn80))** | Базовый TLS-транспорт Goodix `55b4` и reverse engineering протокола. [`libfprint (55b4-exp)`](https://github.com/TheWeirdDev/libfprint) |
| **Matthieu Charette ([@mpi3d](https://github.com/mpi3d))**, **Natasha England-Elbro ([@ashenglandelbro](https://gitlab.freedesktop.org/ashenglandelbro))**, **Timur Mangliev ([@tigrmango](https://github.com/tigrmango))** | Исходный алгоритм SIGFM для небольших ёмкостных сканеров. [libfprint MR !530](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/530) |
| **libfprint upstream** | Библиотека и инфраструктура для работы со сканерами. [`libfprint`](https://gitlab.freedesktop.org/libfprint/libfprint) |

Значительная часть исследования, кода, тестов и документации выполнена с помощью
AI-инструментов под руководством и проверкой человека. Независимый security-аудит
не проводился. Полные сведения об авторстве находятся в [NOTICE](NOTICE).

## Лицензия

[LGPL-2.1-or-later](LICENSE), без гарантий. См. также [NOTICE](NOTICE) и
[SECURITY.md](SECURITY.md).
