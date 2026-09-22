# Code

Inline `ls -la` code.

```bash
#!/bin/bash
for f in *.md; do
  if [[ -f "$f" ]]; then
    echo "$f has $(wc -l < "$f") lines"
  fi
done
```

```c
#include <stdio.h>
int main(void) { printf("hello\n"); return 0; }
```

```ini
[section]
key = value ; comment
```

```
plain fence without a language
┌──────┬──────┐
│ box  │ draw │
└──────┴──────┘
```

~~~sh
echo "tilde fence"
~~~

    indented code block

A reusable snippet. ^snippet
