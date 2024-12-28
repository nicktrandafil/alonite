# Implements a runtime on coroutines

Properties:
* Executors don't leak out of function scopes.
* Doesn't provide means to switch an executor in the middle of a function.
