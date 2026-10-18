# Сопровождение и compiler-isolation CI

Добавлены CONTRIBUTING.md, issue forms (ошибка/предложение), PR checklist,
QUALIFICATION-MATRIX.ru.md. Generated gallery testshots/index.html точечно
помечена linguist-generated=true; PNG/исходники/исторические assets не удалены.

В .github/workflows/build.yml новый отдельный compiler-isolation job:
- read-only permissions, checkout без persist credentials;
- GCC12/multilib/NASM/GRUB/QEMU/OVMF, Python venv с Unicorn2.1.0/Pillow;
- NORMAL SDK/runtime/release, затем host/sanitizer/budget/service/image suites;
- последовательные native CPU, in-OS Workshop и SDK GUI BIOS32/UEFI64;
  во время их исполнения kernel/symbols не пересобираются;
- artifact — только logs/result.json, в том числе при failed run. ISO/disk images/
  .kapp не upload/publish. Существующие storage jobs/release workflow сохранены.

Проверено локально: YAML parsing, permissions/artifact scope, bash -n каждого
нового run блока, issue-form обязательные поля/уникальные IDs и локальные ссылки
новой документации. git check-attr показывает generated только для gallery,
README остаётся unspecified. git diff --check PASS.

Эквивалентные build/host/native команды реально выполнены локально M4.15;
это **не hosted CI success**. Новый workflow не отправлялся и не запускался
на хостинге; нет run URL/conclusion, push/tag/release/publication. Ни версия
stable3.1.3, ни прошлые tags/assets, ни protected chmod не менялись.

Для hosted acceptance после отдельного разрешённого push/PR потребуются
фактический workflow run, commit SHA и conclusion всех нужных jobs. PR #1/#2,
repository topics и дальнейшие публикации этим increment не закрываются.
