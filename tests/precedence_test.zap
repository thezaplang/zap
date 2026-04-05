import "std/prelude" { println };

struct Entry {
    used: Bool,
}

fun main() {
    var ent: Entry = Entry { used: true };
    if !ent.used {
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
