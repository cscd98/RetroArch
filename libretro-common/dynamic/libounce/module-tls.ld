HIDDEN(__libounce_tls_filesz = SIZEOF(.tdata));
HIDDEN(__libounce_tls_memsz = SIZEOF(.main.tls));
HIDDEN(__libounce_tls_align = MAX(16, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))));
