# Parameterized `sql!{...}` example

`prepared.zp` is a small, database-independent demonstration. Import it and
write a query with `${expression}` for each runtime value:

```zap
import "prepared";

var query = prepared.sql!{SELECT * FROM users WHERE name = ${name}};
```

`query.text` contains `SELECT * FROM users WHERE name = ?`; the evaluated
`name` is stored separately in `query.parameters`. The macro never substitutes
the value into the SQL text. A real database adapter must bind these parameters
to its prepared statement API; this example does not execute queries.

The compiler validates `${...}` as Zap expressions, but **does not parse or
validate SQL at compile time**. Placeholder spelling is currently `?`, and
the demo adapter accepts `String` parameters; convert other types explicitly.
