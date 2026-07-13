# User Story 3 Installed Query and Command Demonstration

Date: 2026-07-14  
Executable: `out/build/Debug/Build/Engine/TerraEnginePhysicsGameplayStress.exe`  
Provider: Jolt Physics 5.6.0, revision `e77f175595e64cb44218cc9d9d56fc365ad0e36a`

## Scenario

The installed executable builds one real Jolt world containing 1,024 bodies arranged in a 32 by 32 grid. It shares one provider shape across 256 dynamic bodies and 768 static bodies, queues 256 continuous-force commands from four concurrent producers, merges them at a protected boundary, advances one fixed tick, and issues two 10,000-request query passes through the engine-owned `ThreadPool`.

Each query pass deterministically mixes ray casts, box shape casts, box overlaps, and point queries. Every request passes through the backend-neutral filter and hit-conversion contract; no Jolt headers or provider identifiers are exposed by the executable.

## Installed runs

Command:

```powershell
& 'out/build/Debug/Build/Engine/TerraEnginePhysicsGameplayStress.exe'
```

Two separate installed-process runs both returned exit code 0 and the same stable query ordering hash:

| Invariant | Run 1 | Run 2 |
|---|---:|---:|
| Bodies | 1,024 | 1,024 |
| Commands accepted/applied | 256 / 256 | 256 / 256 |
| Queries per pass | 10,000 | 10,000 |
| Successful queries per pass | 10,000 | 10,000 |
| Queries with hits per pass | 10,000 | 10,000 |
| Hits per pass | 10,000 | 10,000 |
| Pass 1 order hash | `2094117251010007947` | `2094117251010007947` |
| Pass 2 order hash | `2094117251010007947` | `2094117251010007947` |
| Pass timings | 123.996 / 115.909 ms | 107.256 / 107.161 ms |
| Stable order | true | true |
| Clean teardown | true | true |

The object UUIDs are deterministic, so the hash proves same-build/same-platform repeatability across processes as well as between passes.

## Enforced bounds

- Maximum requests per batch: 10,000
- Maximum retained hits per query: 64
- Maximum cast distance: 100,000 metres
- Maximum shape extent: 1,000 metres

The demonstration returned nonzero on any failed query, missing hit, command rejection, ordering mismatch, unhealthy backend, or teardown error. None occurred.

