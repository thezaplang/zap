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