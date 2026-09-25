# Running alongside DLSS5VKLayer

Both ShaderGlassVk and DLSS5VKLayer hook `vkQueuePresentKHR`, and both compose into the swapchain
image and then call down the chain. So whichever of them runs **last** has its work land on top of
the other's, and the order is not a matter of taste:

* DLSS5VKLayer reconstructs and upscales the frame.
* ShaderGlassVk applies a CRT shader, which belongs on the finished picture.

Run the wrong way round, DLSS is handed an already-shadered image to reconstruct -- scanlines, mask
and all -- which is not what either layer is for. The order we want is:

```
app -> DLSS5VKLayer -> ... -> ShaderGlassVk -> driver
```

## Why it was the wrong way round

DLSS5VKLayer's installer writes `~/.config/environment.d/dlssnr.conf`:

```
VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"
```

That makes DLSS an **explicitly** enabled layer in every process of the session, and the loader places
explicit layers below implicit ones. ShaderGlassVk, enabled implicitly through `SHADERGLASS=1`, was
therefore always above it -- and composed first.

This took a while to find, and the path is worth recording. With DLSS explicit, nothing done to
ShaderGlassVk's *implicit* manifest can move it below DLSS, so every experiment on the manifest --
renaming the file, renaming the layer, moving it between directories, wrapping it in a meta-layer --
gave the same answer, which read as the loader being immovable. It was not immovable; the experiments
were adjusting the wrong layer. The session environment was the cause all along.

## The fix

`tools/local-install.sh` appends ShaderGlassVk to the same list, after whatever is already there, in
`~/.config/environment.d/zz-shaderglass.conf`:

```
VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:+${VK_INSTANCE_LAYERS}:}VK_LAYER_SHADERGLASS:VK_LAYER_SHADERGLASS_32"
```

Named after DLSS in the same list, ShaderGlassVk is explicit too and comes later -- closer to the
driver -- so it composes last. The file sorts after `dlssnr.conf`, which is the order systemd reads
them in, so DLSS's value is there to append to; without DLSS installed the expansion yields just the
two ShaderGlass names, with no stray separator. Both behaviours were checked by running systemd's own
environment generator against the files, not assumed from the documentation.

Both architectures' layer names are listed. A process loads the one matching its word size and skips
the other silently.

It takes effect at the next login. `environment.d` is read once, when the session starts, and programs
already running -- Steam among them -- keep the environment they started with.

### What that costs, and how it is kept quiet

Named in `VK_INSTANCE_LAYERS`, the layer is loaded into every Vulkan program in the session, exactly as
DLSS5VKLayer already is. It does nothing unless `SHADERGLASS=1` is set in that program, so the
one-variable switch is unchanged. While switched off it prints nothing at all -- no banner, no device
line, no skip reason -- because a line in the stderr of every Vulkan program on the machine would be
noise from a layer nobody turned on. If `SHADERGLASS` is set to anything other than `1`, the banner
still appears: that is someone who meant to enable it and mistyped, and the banner is how they notice.

`SHADERGLASS_DISABLE=1` is honoured by the layer itself, not only by the manifest, because a manifest's
`disable_environment` governs implicit enabling and has no say over a layer that was named.

## How to tell which way round you are

The layer works it out and says so once per device. It asks the next link in the chain for
`vkQueuePresentKHR` and hands the answer to `dladdr`:

* resolves inside DLSS5VKLayer's object -- DLSS is directly below, and will compose last:

  ```
  [layer] DLSS5VKLayer runs after ShaderGlass and will reconstruct an already-shadered image
  ```

* resolves to the driver -- nothing is below this layer, so DLSS must be above:

  ```
  [layer] DLSS5VKLayer runs first; the shader lands on its output
  ```

Whether the next link is a driver or another layer is asked of the object, not guessed from its name:
every layer exports `vkNegotiateLoaderLayerInterfaceVersion`, because the loader will not load it
otherwise, and a driver does not. An earlier version kept a list of driver library names and missed
NVIDIA's actual one, `libnvidia-glcore`, reporting a correctly ordered chain as undecided.

`SHADERGLASS_VERBOSE=1` adds the object the verdict rests on. One case stays undecided on purpose:
another layer directly below ShaderGlassVk, with DLSS possibly further down behind it.

## Verified

On NVIDIA 615.71.09, loader 1.4.357, with `vkcube` presenting through a real swapchain and the
environment the session produces after login:

| configuration | verdict | frames composed |
| --- | --- | --- |
| after login, `SHADERGLASS=1` | DLSS runs first | 60 of 60, through the 19-pass `crt-royale-smooth` |
| before login (DLSS's order only), `SHADERGLASS=1` | DLSS runs after -- warned | -- |
| after login, `SHADERGLASS` unset | silent, 0 lines | none, as intended |
| after login, `SHADERGLASS=1 SHADERGLASS_DISABLE=1` | idle | 0 |

Not yet confirmed: that DLSS5VKLayer's own neural pass and ShaderGlassVk's chain both run on the same
frame in a game, so the combined picture has been seen rather than inferred.
