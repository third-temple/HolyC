// Adapted from .ref/holyc-docs/HC/Directives.HC
I64 Main()
{
  I64 line = __LINE__;
  I64 cmd = __CMD_LINE__;
  return line + cmd;
}
