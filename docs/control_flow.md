# Control Flow

Zap provides simple and powerful control flow structures.

## While Loops
The `while` loop repeatedly executes a block of code while a condition is true.

```zap
var i: Int = 0;
while i < 10 {
    i = i + 1;
}
```

## Conditionals
Zap supports conditional control flow and conditional expressions.

```zap
if age >= 18 {
    // some logic here
} else {
    // some logic here
}
```

## Ternary Operator
Zap also supports a ternary operator for conditional expressions.

```zap
var age: Int = 18;
var status: String = age >= 18 ? "adult" : "minor";
```

The condition must have type `Bool`, and both branches must have compatible types.

```zap
var points: Int = is_bonus ? 100 : 50;
```
