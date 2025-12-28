; ModuleID = 'zap_module'
source_filename = "zap_module"

define i32 @main() {
entry:
  %a = alloca i32, align 4
  store i32 5, ptr %a, align 4
  %a1 = load i32, ptr %a, align 4
  %a2 = load i32, ptr %a, align 4
  %0 = mul i32 %a1, %a2
  store i32 %0, ptr %a, align 4
  %a3 = load i32, ptr %a, align 4
  %1 = add i32 %a3, 1
  ret i32 %1
}
