# Code Style

Use pascal-case for filenames. 

All header files should have a header comment giving:
- Filename
- Brief description
- @author line
- @License line (reference LICENSE)

*.cpp files have a simple header:
- Filename
- See header file for details.

Do NOT prefix classes/structs/enums/variables with type indicators (so no C<classname> or I<classname>, or k<constname>). 

# Documentation

Documentation should be focused on how the API/function/class/method is used. Think "what does a developer who wants to use this need to know?" Exclude implementation details, and do NOT mention which modules use something. Favour simple language over jargon and large complex words.

Functions and code internal to source-files should also be documented (e.g., static functions).