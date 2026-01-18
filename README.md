jvx is a fast JPEG Viewer. It uses multiple CPUs to decompress JPEGs in
parallel. It works several images ahead of the one which is being viewed.

Other simple image viewers that I've used generally max out at 5-10
images/second. jvx can display >100 images/second even on modest desktop
hardware. If you are viewing images from a memory card, the card or
your keyboard repeat rate will be the bottleneck, not jvx.

## Usage

	jvx <files or directories>

	--memory-footprint=<bytes> - Limits the memory being used to cache
				     images, both the JPEG itself (in the
				     OS page cache) and the decoded pixels.
				     Defaults to 1GB.

## Keyboard Bindings

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

## Performance

jvx does two basic things to go fast:
 1. It reads from the disk (or memory card) as fast as it can
 2. It avoids reading the same thing from the disk twice

### Maximizing Disk Performance

jvx reads files sequentially to make I/O efficient and ensure that the next
image in the list is next one to be read.

### In-Memory Caching

jvx mmap()s the files and keeps them mmap()ed. This avoids bottlenecks
in the operating system.

jvx keeps track of the memory it uses for these mappings and for the
"surfaces" used to store decoded JPEG pixels. It will free the
surfaces and the mmap() backing pages to stay under its target memory
footprint. However, the mmap() itself is only freed at program exit.
This is because munmap() is a heavyweight operation and takes
process-wide locks while madvise(MADV_DONTNEED) is cheaper and
parallelizes better.

Note that the mmap() backing pages are not actively kept in jvx
memory. They are part of the OS "page cache" and the OS might reclaim
them at any time. This means that jvx might use less memory that you
specify with --memory-footprint in cases where the OS needs the memory
for other things. jvx plays nice with its neighbors.

## Keyboard Action Example

Add an action. This will move images to a "keep" directory. The eventual command-line
for actions are built from the literal strings passed in the action arguments. The
only thing that is interpreted or changed is the string "{}" which is replaced with
being viewed when the action key is pressed. This keeps the handling
simple and means that there is no complexity from parsing or shell escaping.

		jvx     --action1="mv"	\
			--action1="$HOME/keep"	\
			--action1="{}"	\
			--action2="ls"	\
			--action2="-l"	\
			--action2="{}"	\
			*.jpg

## History

### Inspiration

I've used [feh](https://feh.finalrewind.org/) for many years. jvx is very much
inspired by feh. But feh can only view an image or two per second. Now that I
have a camera that shoots 30 fps, I can easily shoot 10,000+ JPEGs a day. I
don't want to spend 5,000 seconds looking through those.

I first tried to make feh faster by parallelizing image decoding. But I ended
up with lots of crashes and concluded that the "imlib2" that it uses is not
thread safe and that it's rather deeply integrated with feh. I decided to start
from scratch.

### Starting Over

I tried vibe coding a replacement. It's not *that* much code, so I tried having
ChatGPT generate the entire program. It did a pretty respectable skeleton,
especially handling the GUI and JPEG decoder code. But it did have lots of bugs
and as I made the prompt more complicated, it started losing context and I
got in a viscious circle, so I moved away from having the entire .c file
generated and starting doing it the old fashioned way.
