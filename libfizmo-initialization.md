

**libfizmo initialization**  
_2011-08-23_




**Overview**


There are six initialization steps:


 - `fizmo_register_screen_interface`
 - `fizmo_register_sound_interface`
 - optional: `parse_fizmo_config_files`
 - `fizmo_start`
 - `parse_fizmo_config_files` if not happened before
 - implicit: `link_active_interface_to_story`


fizmo config files:


 - `/etc/fizmo.conf`, always parsed.
 - All `fizmo/config` files in all directories from colon-spearated `XDG_CONFIG_DIRS` path.
 - `$HOME/.config.fizmo`


---

**Color initialization**


The screen default colors are stored in variables `default_foreground_colour` and `default_background_colour` inside `output.c`. By default, the foreground is initialized as white, the background as black. Initialization is as follows:


 1. By default, the foreground color is set to white, the background to black in `output.c`.
 2. The screen interface's functions `get_default_foreground_colour` and `get_default_background_colour` are evaluated. In case these return valid z_colour values, these are used as the default colors. That means that by returning -1 for one or both of the functions, the screen interface may choose not to alter the interpreter's default values.
 3. The `foreground-color` and `background-color` config variables are evaluated from the configuration file(s).
 4. The `fizmo-start` function's variables `screen_default_foreground_color` and `screen_default_background_colour` are evaluated. In case these contains valid z_colour values, they're used as the new default. As in step 2, that means that the default, as evaluated up to this step, may be kept by setting one or both of the values to -1.


Why the additional step 4? While it's already possible to read color information from the screen interface in step 2, step 4 ensures that it's possible to still override the information parsed from the config files in step 3 from the screen interface.

