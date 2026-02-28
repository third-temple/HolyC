#exe {
  Option(OPTf_WARN_PAREN, OFF);
  Cd("/tmp");
  if (1) {
    StreamPrint("I64 GeneratedByExe(){ return 11; }\n");
  } else {
    StreamPrint("I64 GeneratedByExe(){ return 0; }\n");
  }
}

I64 Main()
{
  return GeneratedByExe();
}
