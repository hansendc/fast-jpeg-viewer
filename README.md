jvx is a fast JPEG Viewer. It uses multiple CPUs to decompress JPEGs in
parallel.  It works several images ahead of the one which is being viewed.

Other simple image viewers that I've used generally max out at 5-10
images/second. jvx can display >100 images/second even on modest desktop
hardware. If you are viewing images off a SD card, the card or your keyboard
repeat rate will be the bottleneck, not jvx.

== Usage ==

	jvx <files or directories> 

	--memory-footprint=<bytes> - Limtis the memory being used to cache
                                     images, both the JPEG itself (in the
                                     OS page cache) and the decoded pixels.
                                     Defaults to 1GB.

=== Keyboard Bindings ===

|   Key   | Actions  |
| :-----: | :--------: |
| Left  | Next Image | 
| Right | Previous Image | 
| Page Down | Skip forward 10 images | 
| Page Up | Skip back 10 images |
| Home | Go to first image |
| End  | Go to last image |
| Shift+  | Move images x2 |
| Control +  | Move images x4 |
| Alt +  | Move images x8 |
| Delete | Delete image from viewing list. Does not delete the file. |
| 0-9    | Run action N |
| q | Quit |

== Performance ==

jvx reads files sequentially to make I/O efficient and ensure that the next
image in the list is next one to be read.

jvx mmap()s the files and keeps them mmap()ed. It keeps track of the memory
it uses for these mappings and for the "surfaces" used to store decoded JPEG
pixels.  It will free the surfaces and the mmap() backing pages to stay
under its target memory footprint. However, the mmap() itself is only freed
at program exit. This is because munmap() is a heavyweight operation and
takes process-wide locks while madvise(MADV_DONTNEED) is cheaper and
parallelizes better.

Add an action. This will move images to a "keep" directory. The eventual command-line
for actions are built from the literal strings passed in the action arguments. The
only thing that is interpreted or changed is the string "{}" which is replaced with
being viewed when the action key is pressed. This keeps the handling
simple and means that there is no complexity from parsing or shell escaping.

		jvx     --action1="mv"	\
			--action1="{}"	\
			--action1="$HOME/keep"	\
			--action2="ls"	\
			--action2="-l"	\
			--action2="{}"	\
			*.jpg

 
