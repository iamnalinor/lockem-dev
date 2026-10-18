# lockem — Handoff

Передача работы другому агенту. Здесь: что это, что уже сделано, какие решения зафиксированы
(не пересматривать без причины), что осталось, как собирать и тестировать, и инварианты надёжности.

---

## 1. Что это

Magisk-модуль **lockem** для личного **Pixel 9a (Android 16)**. По заданной комбинации аппаратных
кнопок (power/volume) **обратимо прячет** выбранные приложения, по обратной комбо — возвращает.
Механизм скрытия — штатный и обратимый: `pm disable-user` (+опц. строгий `pm disable`) и `pm hide`;
возврат — `pm unhide` + `pm enable`. Демон читает evdev **пассивно**; эксклюзивный захват (EVIOCGRAB)
используется **только на момент записи комбо**. Это **сокрытие/обфускация, не криптозащита** (см. §7).

Исходный проектный план (с обоснованиями решений) лежит рядом — `PLAN.md`. Он **не вполне актуален**
(источник истины: код → этот HANDOFF → PLAN), держи его ради контекста и «почему». Ключевые решения
также сведены в §2 и §7 ниже.

Окружение разработки/целевое: Magisk **Alpha** + **KsuWebUI** (standalone-рендерер WebUI, его же
использует TrickyStore). Целевое устройство — Pixel 9a, Android 16. Собиралось на Windows + Android
Studio; NDK ставится с Android Studio (путь — в `$ANDROID_NDK_HOME` или `$ANDROID_SDK_ROOT/ndk/<version>`),
версия при разработке 30.0.16248370 — подойдёт любая современная (r26+). Тестовый AVD — образ **x86_64,
API android-37.x, Play Store (ps16k, 16 KB page size)** (про рут на нём — см. §6).

Репозиторий: https://github.com/iamnalinor/lockem-dev (ветка `main`).
Коммить от имени `iamnalinor <63642425+iamnalinor@users.noreply.github.com>`.
В конце commit-месседжей: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## 2. Зафиксированные решения (НЕ пересматривать без причины)

1. **Пассивное чтение evdev**; постоянного grab и uinput **нет нигде**. EVIOCGRAB — только короткое окно
   записи комбо. Побочная реакция системы на комбо при срабатывании допустима (громкость, power-menu).
2. **Паттерн = упорядоченный рецепт токенов**: `click:KEY`, `hold:KEY` (порог `hold_ms`, деф. 500мс),
   и для параллельных нажатий `press:KEY` … `unpress:KEY`. Ключи: `KEY_POWER/KEY_VOLUMEUP/KEY_VOLUMEDOWN`.
3. **Ось disable настраиваемая** (`strict_disable`): по умолчанию `pm disable-user` (надёжно, per-user);
   `strict_disable=1` → пробуем `pm disable` из root, при ошибке фоллбэк на `disable-user`.
   Главная ось скрытия — всегда `pm hide`.
4. **Ребут нейтрален**: `service.sh` lock **не переприменяет** (не ломаем загрузку); состояние hide/disable
   персистентно само. Авто-lock — только по screen-off/таймауту (опции), не на boot.
5. **Опция «отключать Dev Options»** = `settings put global development_settings_enabled 0`.
   **adb НЕ рубим** (`adb_enabled` не трогаем) — чтобы сеть восстановления через adb-su жила.
6. **boot-combo escape** — единственный строимый фоллбэк восстановления: держишь `escape_key` (Vol+)
   в окне старта демона → `unlock` всех пакетов (включая Magisk-app/KsuWebUI, если они в списке → UI
   возвращается). Проверяется демоном при старте (после первой разблокировки lock-скрина, т.к. BFU).
7. **CLI `lockem unlock` самодостаточен** (без демона, прямые `pm`) — для восстановления из adb-su /
   recovery / GSI. Остальные подкоманды идут через демон (флаг-файлы + inotify).
8. **Кроссплатформенность**: стандартный формат Magisk-модуля (Magisk/KernelSU/APatch), WebUI-мост с
   автодетектом (`ksu.exec` / `kernelsu` / MMRL), всё через штатные `pm`/`cmd`/`am`.
9. **Надёжность — приоритет**: ничего не должно вызывать bootloop/нестабильность (см. §7 инварианты).
10. Комбо **не шифруем**, храним как есть (осознанное решение; см. модель угроз §7).

---

## 3. Текущее состояние репозитория

### Готово и закоммичено
- `module.prop`, `default-config` — метаданные и дефолтный конфиг (флаги, паттерны, escape).
- `customize.sh` — выбор бинаря по `$ABI` (arm64-v8a→`lockemd-arm64`, x86_64→`lockemd-x86_64`),
  создание `/data/adb/lockem/` (+`ctl/`), раскладка конфига, права.
- `service.sh` — late_start: ждёт boot_completed, запускает демон в фоне. **(простая версия — см. TODO)**
- `uninstall.sh` — best-effort откат (unhide+enable всех пакетов, restore dev-opts). Примечание: Magisk
  запускает его рано, pm может быть недоступен → надёжный откат = `lockem unlock` на живой системе.
- `sepolicy.rule` — минимальные allow на чтение `/dev/input` (магиск-домен и так permissive).
- `system/bin/lockem` — CLI: `lock`/`unlock`/`reload`/`status`/`record-lock-combo`/`record-unlock-combo`.
  `unlock` самодостаточный. **(нет `log`-подкоманды и валидации пакетов — см. TODO)**
- `src/`:
  - `defs.h` — общие типы (config_t, pattern_t, token_t), пути, прототипы.
  - `util.c` — лог: файл `/data/adb/lockem/lockem.log` с ротацией (cap 256KB, 1 бэкап `.1`) + зеркало в
    logcat (tag `lockem`, под `#ifdef __ANDROID__`).
  - `config.c` — парсер flat-конфига (key=value) + `packages.list`; `pattern_parse`/`pattern_to_string`;
    `key_from_name`/`key_name`.
  - `input.h`/`input.c` — перечисление устройств по capability, пассивное чтение (poll),
    `input_grab` (EVIOCGRAB), `input_key_down` (EVIOCGKEY), `input_read_event`, `now_ms`.
  - `pattern.h`/`pattern.c` — матчер (рецепт→ожидаемые переходы, тайминг委ые click/hold, reset по idle)
    и `pattern_record_normalize` (сырые события → токены, с детекцией wrapper-клавиши для press/unpress).
  - `actions.c` — `act_lock`/`act_unlock`: per-pkg lock/unlock, blocklist критичных пакетов, strict
    disable с фоллбэком, вибро/Private Space/lockdown/dev-opts, сохранение/restore dev-opts в `state`.
    **(НЕТ валидации имён пакетов `valid_pkg` — см. TODO, важно)**
  - `lockemd.c` — main: load config, open input, boot-escape, inotify на `ctl/`, основной poll-цикл
    (матч lock/unlock, обработка ctl, авто-lock housekeeping), запись комбо (`do_record`).
    В `actions.c` теперь есть `valid_pkg()` (`[A-Za-z0-9._]`) — применяется в `lock_one`/`unlock_one`.
  - `jni/Android.mk` + **`jni/Application.mk`** — `APP_ABI := arm64-v8a x86_64`, `android-26`,
    `APP_STL := none`, 16KB page alignment.
- **`build.sh`** — ищет NDK (`$ANDROID_NDK_HOME` → `$ANDROID_SDK_ROOT/ndk/<highest>` → PATH),
  зовёт `ndk-build` (поддержка `.cmd` на Windows), копирует в `bin/lockemd-arm64`/`-x86_64`.
- **`pack.sh`** — собирает `dist/lockem.zip` (+`.sha256`) из бинарей+скриптов+`system/`+`webroot/`.
  Формат — установка через менеджер (без META-INF; см. ниже про flashable).
- **`system/bin/lockem`** — добавлены: валидация имён пакетов в `unlock` (`case` с запретом спецсимволов),
  подкоманда `log [N]` (`tail`).
- **`service.sh`** — переписан: kill-switch `/data/adb/lockem/disable` (проверяется на boot и между
  рестартами); супервайзер с capped backoff (2→60с) и лимитом 8 быстрых падений → сдаётся; всё в фоне.
- **`tests/`** — host unit-тесты: `test.h` (мини assert-раннер), `test_pattern.c` (66 проверок:
  keymap, parse/to_string round-trip incl. press/unpress, матчер click/hold/reset/parallel/empty,
  normalize clicks/hold/parallel, normalize→match), `run_tests.sh` (gcc +ASan/UBSan).
- **`.clang-format`** — стиль C (LLVM-base, 4 пробела, 100 кол, pointer-right, SortIncludes:false).
- **`webroot/`** — `index.html`/`app.js`/`style.css`: список `pm list packages -3` + чекбоксы,
  запись двух комбо (флаг `ctl/record_*` → поллинг `record_result`), ручной конструктор рецепта,
  опции, предупреждения (power+vol-down, dev-opts). Мост `sh()` с автодетектом `ksu.exec`. Пишет
  config/packages.list через `base64 -d` (без проблем с экранированием), затем `ctl/reload`.
- **`.github/workflows/ci.yml`** — jobs `lint` (shellcheck+cppcheck+clang-format), `test` (host-тесты),
  `build` (`nttld/setup-ndk@v1` r26d → `build.sh` → `pack.sh` → upload-artifact),
  `release` (на тег `v*`, `attest-build-provenance@v2` + `gh release create` с zip+sha256).
- `.gitignore`, `LICENSE` (MIT, iamnalinor 2026), `README.md`, `docs/RISKS.md`, `docs/TESTING.md`.

### Проверено в облачном окружении (без устройства/NDK)
- **Host-компиляция** всех `src/*.c` с `-D_GNU_SOURCE -Wall -Wextra`: чисто, без ворнингов, линкуется.
  (На хосте нужны `-D_GNU_SOURCE`/`-std=gnu11` — glibc прячет `strtok_r`/`clock_gettime`/`O_CLOEXEC`/
  `popen`/`strdup`/`usleep` под strict c11; bionic/NDK их открывает сам, так что для NDK это не проблема.
  `run_tests.sh` уже ставит `-D_GNU_SOURCE`.)
- **Unit-тесты**: `PASSED: 66 checks, 0 failures` под ASan/UBSan.
- **Линтеры**: `clang-format --dry-run --Werror`, `cppcheck --enable=warning,portability`,
  `shellcheck` (sh+bash) — всё чисто.
- **`pack.sh`** проверен на заглушках-бинарях: раскладка zip корректна.

### Осталось (нужно устройство/NDK/CI — я к этому доступа не имел)
- **Реальная сборка демона** `ndk-build` (NDK в облаке нет) — собрать `build.sh` локально/в CI.
  Host-компиляция прошла чисто, так что C-ошибок быть не должно, но подтвердить на NDK.
- **CI не запускался** (у пользователя кончились лимиты). Воркфлоу написан, синтаксис YAML валиден;
  прогнать когда появятся лимиты. Если `nttld/setup-ndk` не устроит — заменить на свой NDK-setup.
- **Track A (эмулятор)** и **Track B (Pixel 9a)** — см. `docs/TESTING.md`. Главное для телефона:
  собрать реальные `avc: denied` и довести `sepolicy.rule`; проверить `pm hide` в Settings→Apps на A16,
  реальные вибро/lockdown/recents/Private Space, `strict_disable` из root, тайминг boot-escape.
- **flashable-zip (META-INF)** — сейчас zip рассчитан на установку через менеджер-апп (Magisk/KernelSU/
  APatch ставят модуль-zip напрямую). Если нужен flash из кастом-рекавери — добавить
  `META-INF/com/google/android/update-binary`+`updater-script` (Magisk-version-specific, не делал).
- **superpowers-ревью** (пользователь просил в исходном плане) — опционально прогнать `code-review`/
  `superpowers:requesting-code-review` по финальному диффу.

---

## 4. Статус доделки

Шаги 1–7 исходного плана выполнены и проверены на хосте (см. выше). Остаётся то, что требует
устройства/NDK/CI (см. «Осталось»): реальная NDK-сборка, прогон CI, смоук на эмуляторе/телефоне,
доводка sepolicy, опц. flashable-zip и superpowers-ревью.

---

## 5. Сборка

NDK ставится с Android Studio (в нём есть `ndk-build`, на Windows — `ndk-build.cmd`).
Локально из корня репозитория:
```bash
# укажи путь к своему NDK (внутри Android SDK, каталог ndk/<version>):
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/<version>"
bash build.sh    # -> bin/lockemd-arm64, bin/lockemd-x86_64
bash pack.sh     # -> dist/lockem.zip (+ .sha256)
```
`build.sh` сам найдёт NDK (`$ANDROID_NDK_HOME` → `$ANDROID_SDK_ROOT/ndk/<highest>` → PATH).
Для эмулятора нужен **x86_64**; для телефона arm64. ABI эмулятора — x86_64, образ 16KB-page (ps16k)
→ флаг `-Wl,-z,max-page-size=16384` обязателен (уже в Application.mk).

Host-тесты (Linux/CI, не нужны ни устройство, ни NDK): `bash tests/run_tests.sh`
(gcc `-D_GNU_SOURCE` +ASan/UBSan; собирает `tests/test_pattern.c` + `src/pattern.c` + `src/config.c`).

---

## 6. Тестирование (два трека)

**Важно (caveat по эмулятору):** текущий AVD — **Play Store** образ, его `adb root` не даёт. Для рута на
эмуляторе нужен **Google APIs (НЕ Play Store)** x86_64 образ и рутование через **rootAVD**
(github.com/newbit1/rootAVD, патчит ramdisk Magisk-ом). Также многие эмулятор-образы идут **SELinux
permissive** → AVC-денайлы там НЕ всплывут, но всплывут на телефоне (enforcing). Это всё надо расписать в
`docs/TESTING.md`.

Track A (VM, сейчас) — надёжно проверяет: `pm hide/unhide/disable/enable`, lock/unlock, конфиг, матчер,
force-stop/recents, Private Space, авто-lock, `uninstall.sh`, наличие evdev-событий power/volume
(`getevent -lp`), пассивное чтение, временный grab на запись. НЕ доверять: sepolicy (permissive), реальное
вибро/биометрию-lockdown, реальное поведение recents/PS на железе.

Track B (реальный Pixel 9a, потом): повторить + закрыть sepolicy (собрать `avc: denied`), реальные
кнопки, вибро, lockdown, видимость `pm hide` в Settings→Apps на A16, Private Space `am stop-user`,
проверить `strict_disable` из root, и какие клавиши прошивка грабит на раннем boot (для escape).

Смоук-прогон после установки zip: демон поднялся (`lockem status`, лог `lockem log`), обычные кнопки
работают; записать комбо через WebUI; lock → приложения пропали из drawer/Settings/recents, опции
отработали; unlock → вернулось; ребут в locked → всё ещё спрятано, демон стартовал, lock не переприменён;
escape (Vol+ при старте) → всё разлочилось; удалить модуль/`lockem unlock` → полный откат.

---

## 7. Инварианты надёжности и модель угроз (КРИТИЧНО — не нарушать)

- **Никаких bootloop.** `service.sh` только в фоне, не блокирует boot; супервайзер с backoff и лимитом;
  kill-switch `disable`. Нет post-fs-data-блокировок. sepolicy — только allow (безопасно).
- **В быту НЕ держим grab** — демон не может «залипнуть» на кнопках. Если демон падает во время записи,
  kernel снимает grab на закрытии fd. Крэш демона безвреден (функция просто не работает).
- **Blocklist критичных пакетов** в `actions.c` (launcher, systemui, android, settings, gms, phone,
  shell) — чтобы hide/force-stop не дестабилизировал систему. Плюс нужна валидация имён пакетов.
- **force-stop НЕ трогает данные** (≠ clear-data) — безопасен; это и есть чистка recents.
- **Модель угроз:** lockem — сокрытие от взгляда/casual-доступа, НЕ криптозамок. Загрузчик разблокирован +
  комбо в плейнтексте (`/data/adb/lockem/config`) → любой с рутом над `/data` (adb-su, recovery/GSI через
  `fastboot boot`) читает комбо или снимает hide. Скрытые пакеты перечислимы (`pm list packages -d/-a`,
  `dumpsys package`→`hidden=true`). Любой путь восстановления владельца = путь отката для оппонента. Это
  неустранимо в данной постановке; зафиксировать честно в README/RISKS.
- **Восстановление, если забыл комбо** (в RISKS.md, по порядку): обратная комбо → boot-combo escape
  (Vol+ при старте) → pre-authorized `adb shell su -c 'lockem unlock'` → `lockem unlock` из любого
  root-шелла → `fastboot boot <root-capable GSI/recovery>` для доступа к `/data` (НЕ тот же патченный
  init_boot — он даёт то же состояние; детали метода под Pixel 9a продумать позже). Сам демон делает
  unlock и возвращает Magisk-app/KsuWebUI → UI восстанавливается.
- **Логи ограничены** (256KB + 1 ротация) — не выжирают память.

---

## 8. Формат конфига (flat; читают демон, CLI и WebUI)

`/data/adb/lockem/config` — `key=value` (см. `default-config`): `strict_disable`, `opt_vibrate`,
`opt_private_space`, `opt_lockdown`, `opt_disable_devopts`, `opt_autolock_screenoff`,
`opt_autolock_timeout`, `autolock_timeout_min`, `hold_ms`, `escape_key`, `escape_window_sec`,
`lock_pattern`, `unlock_pattern` (токены через запятую, напр. `click:KEY_VOLUMEUP,hold:KEY_VOLUMEDOWN`).
`/data/adb/lockem/packages.list` — пакеты по одному на строку.
`/data/adb/lockem/ctl/` — флаг-файлы управления (inotify): `reload`, `lock_now`, `record_lock`,
`record_unlock`. `/data/adb/lockem/record_result` — демон пишет результат записи, WebUI поллит.
`/data/adb/lockem/state` — сохранённое прежнее значение dev-opts для restore.
