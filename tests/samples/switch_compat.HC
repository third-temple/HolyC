I64 Main()
{
  I64 x=0;

  switch (x) {
    case:
      "zero\n";
      break;
    case 2...4:
      return 9;
    start:
      "[";
      case 6:
        "six\n";
        break;
    end:
      "]";
      break;
    default:
      return 5;
  }

  return 1;
}
