# bCNC на Windows: отдельное окружение

Для этой установки используется Python 3.11 с Tcl/Tk 8.6, bCNC 0.9.16,
NumPy 1.26.4 и OpenCV 4.5.5.64. Зависимости закреплены в `bcnc-requirements.txt`.
Полный набор установленных версий — `bcnc-requirements.lock.txt`.
Сочетание Python 3.15/Tk 9 и NumPy 2 с прежним OpenCV вызывает показанные
ошибки `trace variable` и NumPy ABI. Само сообщение «Python 3.8 and newer»
не гарантирует совместимость старого кода с Tk 9.

Открывать bCNC через **Start-bCNC.cmd** рядом с этим файлом. Активация venv
и изменение ExecutionPolicy не нужны. Запускать из PowerShell можно так:

```powershell
& 'C:\Users\Denis Nekoz\Documents\Arduino\WaterPressureSensor\hardware\cnc\Start-bCNC.cmd'
```

Окружение расположено вне исходников проекта, в `%LOCALAPPDATA%\bCNC\venv311`.
Отдельный профиль настроек: `%LOCALAPPDATA%\bCNC\bcnc.ini`.
Команда `python -m bCNC` по-прежнему может выбрать системный Python 3.15;
использовать указанный файл запуска или точный путь к Python окружения.

Воспроизведение установки, если требуется:

```powershell
py install --yes 3.11
py -3.11 -m venv "$env:LOCALAPPDATA\bCNC\venv311"
& "$env:LOCALAPPDATA\bCNC\venv311\Scripts\python.exe" -m pip install -r 'C:\Users\Denis Nekoz\Documents\Arduino\WaterPressureSensor\hardware\cnc\bcnc-requirements.lock.txt'
& "$env:LOCALAPPDATA\bCNC\venv311\Scripts\python.exe" -m pip check
```

После открытия программа остаётся без подключения к порту до нажатия Open.
Проверка запуска не означает проверки связи с GRBL, probing или режимов резания.

Проверено 18.10.2026: Python 3.11.9, Tk 8.6.12, импорт OpenCV и операция над
массивом NumPy, старый API trace, создание интерфейса bCNC, загрузка воздушной
УП и интерполяция синтетической карты высот. COM-порты при проверке были
заблокированы. `pip check` — без конфликтов. Отчёт — `bcnc-startup-check.json`.
