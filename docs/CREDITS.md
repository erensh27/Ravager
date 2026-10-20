# Credits

Ravager's search and handcrafted evaluation build on ideas published and
refined by the computer-chess community over decades. Ravager's engine code
is maintained in this repository.

## Bundled components

- [Pyrrhic](https://github.com/AndyGrant/Pyrrhic), by basil, Jon Dart, and
  Andrew Grant: Syzygy probing. MIT; the full license text remains with the
  component at [src/tb/LICENSE](../src/tb/LICENSE).
- [incbin.h](https://github.com/graphitemaster/incbin), by Dale Weiler:
  optional binary embedding. Public domain.
- [Zurichess tuner data](https://bitbucket.org/zurichess/tuner): quiet-position
  dataset used to tune the handcrafted evaluation.
- Syzygy tablebase format by Ronald de Man.

## Neural network

- The embedded network weights are CC0 1.0; the file in `nets/` is included unchanged.
