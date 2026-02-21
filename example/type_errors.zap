// example/type_errors.zap

fun main() {
    // Valid declarations and assignments
    var a: Int = 10;
    var b: Float = 20.0;
    var c: Bool = true;
    var d: String = "hello";

    var e: Float = a; // Implicit Int to Float conversion

    // Invalid assignment (Int to Bool)
    var f: Bool = 5; // ERROR: Cannot assign expression of type 'i64' to variable of type 'i1'

    // Invalid assignment (Float to Int)
    var g: Int = b; // ERROR: Cannot assign expression of type 'f64' to variable of type 'i64'

    // Invalid binary operation (Int + Bool)
    var h: Int = a + c; // ERROR: Operator '+' cannot be applied to types 'i64' and 'i1'

    // Valid binary operation (Int + Float)
    var i: Float = a + b; // Result should be Float

    // Function calls
    add(10, 20); // Valid

    // Invalid function call (wrong number of arguments)
    add(10); // ERROR: Function 'add' expects 2 arguments, but received 1

    // Invalid function call (wrong type of argument)
    add(10, 20.0); // ERROR: Argument 2 of function 'add' expected type 'i64', but received type 'f64'

    // If condition not Bool
    if (a) { // ERROR: If condition must be of type 'Bool', but received 'i64'
        // ...
    }

    // While condition not Bool
    while (b) { // ERROR: While condition must be of type 'Bool', but received 'f64'
        // ...
    }

    // Valid return type
    var retInt: Int = returnsInt();
    var retVoid: Void; // void type or any other type

    // Invalid return type
    // fun returnsInt(): Int { return true; } // ERROR: Function expects return type 'i64', but received 'i1'.
    // var invalidReturn: Int = returnsVoid(); // ERROR: Function expects return type 'i64', but received 'void'.
}

fun add(x: Int, y: Int) Int {
    return x + y;
}

fun returnsInt() Int {
    return 1;
}

fun returnsVoid() {
    // nothing
}

fun returnsBool() Bool {
  return false;
}

fun testArray() {
    var arrInt: [5]Int;
    // var arrFloat: [3]Float;
    // var arrString: [2]String;

    var initArr: [2]Int = {1, 2}; // Valid
    // var initArrError: [2]Int = [1, 2.0]; // ERROR: Cannot assign expression of type 'f64' to variable of type 'i64'

    // var invalidArrSize: [a]Int; // ERROR: Array size must be a constant integer expression.
}

