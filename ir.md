# simple, mostly example-based (for now), intermediate representation documentation for the Zap programming language

**first example `simple_app.zap`**

```zap
import std.io;
import std.str;

mod Error {
  enum Code {
    SUCCES,
    FAILURE,
    SOMETHING_ELSE_IDK
  }
}

mod consts {
  val PI: float = 3.14;
}
@{printer}
struct User {
  name: str,
  email: str,
  isPremium: bool
}

fun User:new(name: str, email: str, isPremium: bool) : User {
  return {name: name, email: email, isPremium: isPremium};
}

fun User:show() {
  io.log("name :", self.name);
}

fun add(a: int, b: int) : int {
  return a + b;
}

fun main() {
  val a: int = 5;
  var b: int = 5 * 3;
  b = b * 2;
  val user: User = User("Jan", "jan@example.com", true);
  io.log("Hello world");
  user.show();
  io.log(user); //user has @{printer} atributte so it works
  io.log("Sum 2+3: " + add(2,3));
}
```

---

## Module (mod)

```zap
mod Error { ... }
```

- Namespace / prefix. All contained entities are prefixed with `Error_` in IR.
- Example: `Error_Code_SUCCES` → `int`

---

## Enum

```zap
enum Code {
    SUCCES,
    FAILURE,
    SOMETHING_ELSE_IDK
}
```

- Enum represented as integer.
- `Code` values in IR are just integer constants.
- Example: `%c: int = 0 ; Code_SUCCES`

---

## Constants

```zap
val PI: float = 3.14;
```

- Immutable, value embedded directly.
- Example IR: `%pi: float = 3.14`

---

## Structs

```zap
@{printer}
struct User {
  name: str,
  email: str,
  isPremium: bool
}
```

- Struct fields are typed.
- `@{printer}` is ignored at IR level (compile-time only).
- IR represents struct creation and field access explicitly:

```
%user: User = struct User %name, %email, %isPremium
%tmp: ptr i8 = getfield %user name
```

---

## Functions

### User:new

- Constructs a `User` struct.
- IR:

```
func User_new(name:ptr i8, email: ptr i8, isPremium:i1) -> User:
entry:
  %user: User = struct User %name, %email, %isPremium
  return %user
```

### User:show

- Calls `io.log` with the `name` field.
- IR:

```
func User_show(self: User) -> void:
entry:
  %tmp:ptr 18 = getfield %self name
  call std_io_log "name :", %tmp
  return
```

### add

- Adds two integers.
- IR:

```
func add(a:int, b:int) -> int:
entry:
  %sum:int = add %a, %b
  return %sum
```

### main

```
func main() -> void:
entry:
  %a:int = 5
  %b:int = mul 5, 3
  %b:int = mul %b, 2
  %user:User = call User_new "Jan", "jan@example.com", true
  call std_io_log "Hello world"
  call User_show %user
  call std_io_log %user
  %tmp:int = call add 2, 3
  %msg:ptr i8 = concat "Sum 2+3: ", %tmp
  call std_io_log %msg
  return
```
