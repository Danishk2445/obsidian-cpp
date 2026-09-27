# Reading

Books I am reading right now:

```base
filters:
  and:
    - file.hasTag("book")
    - status == "reading"
views:
  - type: table
    name: Now
    order:
      - file.name
      - author
      - pages
```

All of them:

![[Books.base#Gallery]]
