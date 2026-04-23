# mori: Shared Memory for R Objects

Share R objects via shared memory with
[`share()`](https://shikokuchuo.net/mori/dev/reference/share.md), access
them in other processes with
[`map_shared()`](https://shikokuchuo.net/mori/dev/reference/map_shared.md),
using R's ALTREP framework for zero-copy memory-mapped access. Shared
objects serialize compactly via ALTREP serialization hooks. Shared
memory is automatically freed when the R object is garbage collected.

## See also

Useful links:

- <https://shikokuchuo.net/mori/>

- <https://github.com/shikokuchuo/mori>

- Report bugs at <https://github.com/shikokuchuo/mori/issues>

## Author

**Maintainer**: Charlie Gao <charlie.gao@posit.co>
([ORCID](https://orcid.org/0000-0002-0750-061X))

Other contributors:

- Posit Software, PBC ([ROR](https://ror.org/03wc8by49)) \[copyright
  holder, funder\]
