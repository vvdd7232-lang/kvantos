# FPU/context safety design — prerequisites, не включённая поддержка float

**Текущее исполнение:** CPL3 всегда входит с CR0.TS|MP=1 и CR4.OSXSAVE=0. FPU/SIMD/state access
запрещён; M4.15 расширяет отрицательные проверки, но не снимает этот fence.
Компилятор не поддерживает float/double. Default ABI3 остаётся trusted ring0.

## Почему недостаточно снять TS или добавить FNINIT

- FNINIT сбрасывает control/tags, но не гарантирует обнуление физического
  содержимого ST/MM registers. Новый процесс может сохранить даже пустые slots.
- FNSAVE сохраняет x87/MMX, но не XMM/MXCSR. На FXSR CPU user FXSAVE доступен
  независимо от намерения компилятора «генерировать только x87».
- CR4.OSFXSR=0 не является достаточным доказательством сокрытия XMM/MXCSR:
  Intel описывает сохранение этих полей FXSAVE без OSFXSR как implementation
  dependent. Нельзя оставлять kernel/previous-owner SIMD state доступным.
- FXSAVE не пишет software bytes464..511. Буфер обязан быть предварительно
  обнулён, а не сериализовать старую BSS/stack память под видом CPU state.
- FIP/FDP/FOP/selector fields тоже содержат данные. Инициализация/переключение
  должны исключать kernel pointers и vendor-specific stale environment state.
  AMD/Intel особенности требуют отдельной проверки, не предположения по QEMU.
- Включение XSAVE/AVX расширяет набор состояния: XCR0/CPUID0xD, YMM и другие
  components. FXSAVE512 уже недостаточен. В первом FP increment OSXSAVE/AVX
  не включать; не выдавать x87-only compiler за доказательство запрета XSAVE.

## Предлагаемая последовательность реализации (пока OPEN)

1. Отдельный opt-in FP image/profile flag с fail-closed старым loader и CPUID
   gates. Без flag — прежний TS|MP fence. Нет общего silent enable для ABI4.
2. CPUID.FPU/FXSR/SSE policy. Для Pentium BIOS32 без FXSR нельзя выполнить
   FXSAVE: отдельно проектировать108B legacy x87 context или отказывать FP
   profile; не ломать запуск обычных integer apps. Для x64 учитывать XMM8..15.
3. Kernel-owned, aligned, zero-initialized per-owner context; bounded BSS/PMM
   ownership/OOM rollback. Никаких pointers на него в user metadata/copyout.
   Seed должен реально затирать физические доступные register banks и env;
   FNINIT в одиночку не является seed. MXCSR reserved bits — по CPU mask.
4. IRQ-off atomic enter: сохранить prior CR0/CR4 и host CPU state до любого
   изменения live FP registers, установить проверенную user policy и private
   user state. Оставить RDPMC/FSGSBASE/fast-entry fences неизменными.
5. Exit/yield/wait/fault/kill: сначала сохранить (если нужен resume) CPU-owned
   user state, восстановить host state, затем prior CR0/CR4/IRQ/TSS/CR3.
   Не делать FXRSTOR при выставленном TS/EM и не восстанавливать напрямую
   пользовательский buffer из syscall как kernel context.
6. Unmasked pending #MF/#XM принадлежат user. NE/exception routing и save без
   внезапного kernel #MF должны быть определены для каждого поддержанного
   backend. Faulting user FXRSTOR/FRSTOR может частично изменить CPU state:
   обработчик обязан всё равно безопасно восстановить host owner.
7. Один GUI user dispatcher, не SMP. Scheduler/IRQ path не может переключить
   kernel peer при live user FP state без ownership handoff. Если kernel tasks
   начинают использовать FP, им нужен собственный task context; одной записи
   в continuation недостаточно. Проверить kernel/SDK compiler flags и ISR code.
8. Полное обнуление private FP buffers при unload/failure; следующему процессу
   не достаются data/register/environment предыдущего. Host state сначала
   восстановлен, только после этого можно освобождать private pages.

## Обязательные gates до смены default deny

- Native BIOS32 без FXSR и UEFI64 с FXSR: feature negotiation/refusal без #UD
  в kernel, обычные integer приложения и прежние fault suite остаются PASS.
- Различимые ST/MM, FCW/FSW/tags, XMM0..7/15, MXCSR sentinel patterns у host
  и user, успешный syscall/IRQ и >=4 preempt/resume, WAIT/DRAW queue/close.
- Между двумя запусками user: clean seed, нет прошлых значений, FIP/FDP/env
  не раскрывают kernel state; нет stale software bytes в context images.
- Разные rounding/control modes, masked/unmasked divide/invalid, bad/unaligned/
  guard-crossing user save/restore buffers, reserved MXCSR → только user fault.
- Kill во время FP loop, failed load/OOM/unload/reload: kernel state, PMM,
  CR0/CR4/MSRs/TSS/CR3/IRQ/GUI/peer восстановлены, без double free или #MF ядра.
- XSAVE/AVX/неподдержанные banks остаются гарантированно закрыты; отдельные
  probes на state-save инструкции, не только один FLD1.
- Только после context proof: MCC type model/literals/conversions/comparisons,
  internal function ABI, array/struct layout/sizeof и math golden tests.
  Varargs/format и API arguments тоже требуют политики; `%f` не появляется
  автоматически. float/double нельзя передавать через прежний integer ABI.
- QEMU — software proof, не physical/vendor qualification и не security certification.

## Инструкционные справки, прочитанные при design

- [FXSAVE](https://www.felixcloutier.com/x86/fxsave):512B,16-byte alignment,
  software bytes464..511 untouched; OSFXSR-dependent XMM/MXCSR save caveat.
- [FWAIT](https://www.felixcloutier.com/x86/wait:fwait): #NM при MP=1 и TS=1.
- [XSAVE](https://www.felixcloutier.com/x86/xsave): CPUID/OSXSAVE gates, TS #NM,
  EDX:EAX & XCR0,64-byte alignment, header/component format.

Это safety plan, не доказательство готового FP backend. Текущие default fences
не ослабляются ради выполнения roadmap checkbox; physical/storage TT gates
и публикация независимо остаются закрыты до своих условий.


M4.15 native обнаружил, что QEMU10.0.13 при inherited OSXSAVE пропускал
XSAVE/XRSTOR несмотря на TS. Теперь policy явно очищает OSXSAVE и проверяет
#UD6, включая restoration исходного CR4. Это defense-in-depth по наблюдаемому
software path, не утверждение о поведении physical CPU. [Proof](M4.15-XSTATE-FENCES.ru.md).
