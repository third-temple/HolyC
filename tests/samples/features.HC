U0 Test(I64 i=4, I64 j, I64 k=5)
{
  switch (j) {
    case:
      "zero";
      break;
    case 4...7:
      "range";
      break;
    start:
      "[";
      case 1:
        "one";
        break;
      case 3:
        "three";
        break;
    end:
      "]";
      break;
  }
}

Test(,3);
