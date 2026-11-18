# Composing a checkout

Run `checkout.tfd`; check `checkout_models.tfd` as a library. Both files use only
ordinary local source modules, pure functions, records, enums, machines and the
existing prelude. The companion import is `"checkout_models.tfd"`, relative to
the entry file. There is no library search path or parent-directory import.

`Stock` and `Wallet` are separate nominal machine types. `Checkout` pairs their
snapshots. `purchase(Checkout, Order) -> Purchase` first steps stock, then steps
the original wallet only if stock accepted. Both accepts produce a new pair
and concatenate stock messages before wallet messages. Either rejection returns
the **entire original pair** with a typed `CheckoutFault`. The ordinary enum
`Purchase` has `Accepted(Checkout, List[Text])` and
`Rejected(Checkout, CheckoutFault)` variants; rejection has no emission-list
payload. These are source-level choices, not an automatic multi-machine
transaction guarantee.

On wallet rejection, the stock step has already returned an accepted candidate
and its message list. The helper discards both. The rejected wallet step produces
no messages at all. `messages` formats a rejection explanation; this is an
ordinary report string, not a rejected machine emission. The transcript contains
no candidate stock message for the discarded reservation of three units.

Each remainder writer is deliberately written before its prerequisite:
`available = 6 - next.reserved`, `balance = 30 - next.spent`. The only dependency
edges are `reserved -> available` and `spent -> balance`. ASCII ready ordering
gives `prior_reserved, reserved, available` and `prior_spent, spent, balance`.
The `prior_*` cells read the old cumulative cells. Bare `available` and `balance`
in rejection payloads likewise report the original availability and funds.

## Deriving the transcript

The 24-line UTF-8/LF transcript in [checkout.expected.txt](checkout.expected.txt)
was written before the first source execution. Quantities and money are Int;
each charge is `quantity * unit_price`. Initial stock is six, funds thirty.

| Attempt | Stock proposal | Wallet proposal | Published result |
|---|---|---|---|
| Buy 2 at 5 | available 4, reserved 2 | balance 20, spent 10 | Both candidates and two messages |
| From there, buy 1 at 25 | available 3, reserved 3 | balance -5, spent 35 | `CheckoutFault::Wallet(WalletFault::Insufficient(25,20))`; retain 4/2 and 20/10, no proposal messages |
| From there, buy 5 at Int maximum | available -1, reserved 7 | Not evaluated; the charge would overflow | `CheckoutFault::Stock(StockFault::Insufficient(5,4))`; retain both originals |
| From initial, buy 0 at 5 | unchanged candidate | Not evaluated | `CheckoutFault::Stock(StockFault::InvalidQuantity(0))`; retain initial pair |
| Fold from initial: 2 at 5, 1 at 25, 4 at 5 | second proposal discarded; final available 0, reserved 6 | second proposal discarded; final balance 0, spent 30 | Two successes around one rejection; final prior cells 2 and 10 |
| Independently from initial, buy 1 at 3 | available 5, reserved 1 | balance 27, spent 3 | Independent branch; original remains 6/0 and 30/0 |

`advance(Journal, Order) -> Journal` is an ordinary left-fold callback. It keeps
the returned snapshot and appends the result's report lines. A rejected event
therefore leaves the next event's input pair unchanged. The final successful
event reaches both nonnegative guard boundaries exactly. Comparing every field
of retained snapshots in the transcript also covers the old-read audit cells.

## Module contract

All exported types and their exposed component types are public. Callers qualify
them through `shop::`, including nested enum constructors. Public helpers are
`purchase`, `snapshot`, `fault_text`, `messages`, `describe`, and `advance`.
`charge_for(Order) -> Int` is private. Importing the module does not re-export
its names or execute main/I/O. `check` succeeds for the companion without main;
`run` rejects that library root with E208 and no stdout. A caller of
`shop::charge_for` rejects with E206, rather than gaining access to the helper.

The [example manifest](../manifest.json) runs the entry and checks the companion
as a library. The oracle is 1,198 bytes, including the final LF.

## Reproduce on Windows

From the extracted project root, in one PowerShell session, with a supported
C17 toolchain available to the build wrapper:

```powershell
./scripts/build.ps1 build
./build/ternfold.exe check examples/showcase/checkout_models.tfd --root examples
./build/ternfold.exe run examples/showcase/checkout.tfd --root examples
./build/ternfold.exe build examples/showcase/checkout.tfd --root examples --cc $env:TF_CC -o build/checkout.exe
./build/checkout.exe
```

Pass `-Compiler PATH` to the build wrapper if your compiler is outside PATH.
The corresponding sanitizer run is part of `./scripts/build.ps1 sanitize`.
See [testing](../../docs/testing.md) for the interpreter and native checks.

All candidates precede guards. Large arbitrary inputs can fault checked arithmetic
before a domain guard returns a rejection, and `charge_for` is evaluated only
after stock accepts. This bounded demonstration does not add overflow recovery,
durability, concurrency, or retries.
