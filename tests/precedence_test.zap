
struct Entry {
    used: Bool,
}

fun main() {
    var ent: Entry = Entry { used: true };
    if !ent.used {
        // This should NOT be reached if precedence is correct
        // ! (ent.used) -> !true -> false
        // If bug exists: (!ent).used -> Error (Bool doesn't have used) or wrong behavior
        println("BUG: !ent.used evaluated as (!ent).used");
    } else {
        println("OK: !ent.used evaluated as !(ent.used)");
    }

    ent.used = false;
    if !ent.used {
        println("OK: !ent.used evaluated as !(ent.used) when false");
    } else {
        println("BUG: !ent.used evaluated incorrectly when false");
    }
}
