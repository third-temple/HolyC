I64 Main()
{
  I64 x=6;
  I64 out=0;

  switch (x) {
    case 2...4:
      out=out+1;
    start:
      out=out+2;
    case 6:
      out=out+4;
      break;
    end:
      out=out+8;
      break;
    default:
      out=99;
  }

  return out;
}
