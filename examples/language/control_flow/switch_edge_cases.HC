I64 Main()
{
  I64 x=3;
  I64 out=0;

  switch (x) {
    case:
      out=100;
    case 1:
      out=out+1;
      break;
    case 2...4:
      out=out+10;
    case:
      out=out+20;
      break;
    default:
      out=999;
  }

  return out;
}
