# ETS2 Car Enhance v2.1.2 (by Fel1neee)

**Language:** [English](#-english) | [Русский](#-русский)

![ETS2 Car Enhance Cover](ETS2_Car_Enhance_Cover_Fel1neee.jpg)

### 🎬 Gameplay & Physics Test (YouTube)
[![ETS2 Car Enhance Showcase](https://img.youtube.com/vi/8Nq802V1-Jo/maxresdefault.jpg)](https://www.youtube.com/watch?v=8Nq802V1-Jo)

> Watch on YouTube: https://www.youtube.com/watch?v=8Nq802V1-Jo

---

## 🇬🇧 English

Native C++ / DX11 plugin for **Euro Truck Simulator 2** that transforms passenger car and supercar driving physics, adds real-time thermal simulation for tires and brakes, high-speed radial motion blur, and an interactive in-game telemetry HUD & overlay menu.

> **Note:** Automatically disables physics assists on trucks/trailers (`CarClass::Truck`) so vanilla trucking physics stay untouched.

### Key Features
- **Speed-Dependent Aero Downforce:** Quadratic downforce curve with front/rear axle balance (`aero_front_pct`) and vertical anti-bounce damping.
- **Dual-Loop ESP / TC / ABS (`OFF` & `L1–L5`):** Yaw-rate + slip-angle stabilization, speed scrub (`esp_brake_drag`), and high-speed ABS brake assist with clamped **Brake Dive**.
- **Pacejka-Lite Tire Model & Kamm Circle:** Progressive slip curve (`0–3.5°` peak, `3.5–11.5°` controllable edge, `11.5°+` breakaway), Trail Braking turn-in bite, smooth brake understeer, and power oversteer.
- **4 Tire Compounds & 3 Brake Disc Types:**
  - **Tires:** `Slick`, `Semi-Slick`, `Street`, `Narrow Street`.
  - **Brakes:** `Standard` (fade `430°C`), `Sport` (fade `560°C`), `Carbon-Ceramic` (~`86%` cold bite below `150°C`, fades above `780°C`) with real "soft pedal" Brake Fade.
- **Thermal Simulation (`dt` via `QueryPerformanceCounter`):** Cold start synced to in-game time (`~16–27°C`), auto-reset on vehicle change, 4-wheel tire/brake heating & airflow cooling.
- **DX11 Radial Speed Blur & In-Game UI (`Insert` / `F6` / wheel button):**
  - **Basic Mode** (quick pre-drive setup) & **Advanced Mode** (full sliders).
  - **Draggable Mini Telemetry HUD** (scalable `0.70x – 1.80x`): steering angle strip, `ABS`/`TC` status, tire & brake temps, 4-wheel chassis diagram with tread patterns, turning front wheels, and center **G-Force meter**.

### Installation (For Players — No SDK Required)
1. Download `ets2_car_enhance.dll` from [Releases](https://github.com/Fel1neee/ets2-car-enhance/releases) (single standalone `.dll`, no extra frameworks or SDK needed).
2. Copy `ets2_car_enhance.dll` into:
   `<ETS2_DIR>\bin\win_x64\plugins\`
3. Launch ETS2 (64-bit, DirectX 11) with any passenger car mod.
4. Press **`Insert`** or **`F6`** (or bind any steering wheel button in the menu) to open the overlay.

### Tested Car Mods
Works best and tested primarily on high-quality car mods by **DN Modding** and **Nimit**:
- `Lamborghini Huracán LP 580-2`
- `Lamborghini Countach`
- `Chevrolet Corvette`
- `BMW M5`
- `Lexus LS500`
- `Volkswagen Golf 7.5`
- `Dodge Challenger / Charger Hellcat`

### Building from Source
All required headers/libraries (`SCS Telemetry SDK`, `MinHook`, `ImGui`) are already included in `third_party/`.
To compile, you only need **llvm-mingw** (`g++` / `clang++` for Windows x64):
- **llvm-mingw:** https://github.com/mstorsjo/llvm-mingw/releases
- **Official SCS Telemetry SDK (reference):** https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry

```powershell
.\build.ps1
```

---

## 🇷🇺 Русский

Нативный C++ / DX11 плагин для **Euro Truck Simulator 2**, который перерабатывает физику легковых авто и спорткаров, добавляет тепловую модель шин и тормозов в реальном времени, радиальное размытие скорости и интерактивный мини-HUD с оверлей-меню прямо в игре.

> **Важно:** На тягачах и фурах (`CarClass::Truck`) физические ассисты отключаются автоматически — родная физика грузовиков не ломается.

### Основные возможности
- **Аэродинамический прижим (`Downforce`):** прижимная сила с ростом скорости, баланс перед/зад (`aero_front_pct`) и анти-подскок на стыках.
- **Стабилизатор `ESP / TC / ABS` (`OFF` и `L1–L5`):**
  - Двухконтурная стабилизация (по вращению и углу сноса) + реальная потеря скорости при ошибке (`esp_brake_drag`).
  - Усиление тормозов на высокой скорости (`ABS`) + естественный клевок носом (**Brake Dive**).
- **Шинная модель (`Pacejka-лайт + Круг Камма`):**
  - Прогрессивная кривая сцепления: пик `0–3.5°`, широкое окно грани `3.5–11.5°` (есть время поймать машину), срыв после `11.5°+`.
  - Реакция на педали: **Trail Braking** (лёгкое подтормаживание заправляет нос в поворот), плавный увод морды при переборе тормоза и контролируемый занос под газом.
- **4 типа шин и 3 типа тормозных дисков:**
  - **Шины:** `Слик`, `Полуслик`, `Дорожная`, `Узкая дорожная`.
  - **Тормоза:** `Обычные` (плывут с `430°C`), `Спорт` (лучше продуваются, с `560°C`), `Керамика` (`86%` на холодную до `150°C`, держат до `780°C`).
  - При перегреве работает реальный **Brake Fade («ватная педаль»)** — удлиняется тормозной путь.
- **Тепловая модель (`dt` через `QueryPerformanceCounter`):** холодный старт по игровому времени суток (`~16–27°C`), сброс при смене машины в гараже, прогрев внешних колёс в поворотах и охлаждение на прямой.
- **DX11-размытие скорости и интерфейс (`Insert` / `F6` / кнопка на руле):**
  - Два режима меню: **Базовый** (быстрая настройка перед заездом) и **Продвинутый** (все тонкие ползунки).
  - **Перетаскиваемый мини-HUD** (масштаб `0.70x – 1.80x`): верхняя полоса угла руля с точкой, индикаторы `ABS`/`TC`, температуры в °C, схема 4 колёс с протектором, поворот передних колёс и **G-метр с шариком** в центре шасси.

### Установка (Для игроков — SDK скачивать НЕ нужно)
1. Скачай готовый файл `ets2_car_enhance.dll` из раздела [Releases](https://github.com/Fel1neee/ets2-car-enhance/releases) (достаточно **одного этого `.dll` файла**, никакие фреймворки или SDK не нужны).
2. Скопируй `ets2_car_enhance.dll` в папку:
   `<Папка_ETS2>\bin\win_x64\plugins\`
   *(если папки `plugins` нет — создай её вручную)*.
3. Запусти ETS2 (64-bit, DirectX 11) с любым модом на легковую машину.
4. Открой меню клавишей **`Insert`** или **`F6`** (или назначь любую кнопку на руле прямо в футере меню).

### Проверенные моды авто
Адекватнее всего плагин работает и тестировался на модах от **DN Modding** и **Nimit**:
- `Lamborghini Huracán LP 580-2`
- `Lamborghini Countach`
- `Chevrolet Corvette`
- `BMW M5`
- `Lexus LS500`
- `Volkswagen Golf 7.5`
- `Dodge Challenger / Charger Hellcat`

### Сборка из исходников (для разработчиков)
Все библиотеки (`SCS Telemetry SDK`, `MinHook`, `ImGui`) уже лежат в репозитории в папке `third_party/`.
Для компиляции нужен только компилятор **llvm-mingw** (`g++` / `clang++` под Windows x64):
- **Скачать llvm-mingw:** https://github.com/mstorsjo/llvm-mingw/releases
- **Официальный SCS Telemetry SDK (если нужен оригинал от SCS):** https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry

Запуск сборки:
```powershell
.\build.ps1
```
