#define MSG "jit\n"
#include "header.HH"
#ifjit
#exe { StreamPrint("I64 Generated(){ return 7; }\n"); }
#else
I64 Generated()
{
  return 0;
}
#endif

U0 Main()
{
  MSG;
  Included;
  Generated;
}

Main;
