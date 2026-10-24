// The C++ part of libndktest.so: static constructors (.init_array), the
// system libstdc++'s operator new/delete and __cxa_guard_* for
// function-local statics, virtual calls, __cxa_atexit-registered
// destructors. No exceptions or RTTI (as the NDK's default "system" STL).

#include <stdio.h>
#include <string.h>

namespace {

int g_constructed;

struct Global {
  Global() : value(1234) { g_constructed++; }
  ~Global() { value = 0; }
  int value;
};
Global g_global;

struct Shape {
  virtual ~Shape() {}
  virtual int Area() const = 0;
};
struct Rect : Shape {
  Rect(int w, int h) : w(w), h(h) {}
  int Area() const override { return w * h; }
  int w, h;
};
struct Square : Rect {
  explicit Square(int s) : Rect(s, s) {}
  int Area() const override { return Rect::Area() + 1; }
};

int Counter() {
  static int calls = 0;
  static Global local;  // guarded initialization
  return ++calls + local.value;
}

char g_fail[128];

}  // namespace

extern "C" const char* cxx_checks() {
  if (g_constructed != 1 || g_global.value != 1234) {
    snprintf(g_fail, sizeof g_fail, "static constructor: %d %d", g_constructed, g_global.value);
    return g_fail;
  }
  Shape* shapes[2] = {new Rect(3, 4), new Square(5)};
  int total = 0;
  for (Shape* s : shapes) total += s->Area();
  for (Shape* s : shapes) delete s;
  if (total != 12 + 26) {
    snprintf(g_fail, sizeof g_fail, "virtual calls: %d", total);
    return g_fail;
  }
  int* arr = new int[1000];
  for (int i = 0; i < 1000; i++) arr[i] = i;
  long sum = 0;
  for (int i = 0; i < 1000; i++) sum += arr[i];
  delete[] arr;
  if (sum != 499500) {
    snprintf(g_fail, sizeof g_fail, "new[]: %ld", sum);
    return g_fail;
  }
  int a = Counter(), b = Counter();
  if (a != 1235 || b != 1236 || g_constructed != 2) {
    snprintf(g_fail, sizeof g_fail, "local static: %d %d %d", a, b, g_constructed);
    return g_fail;
  }
  return nullptr;
}
