# Generate the official self-contained MicroPython embed package. The source
# repository and commit are fixed by the parent repository submodule.

USER_C_MODULES := $(CURDIR)/usermod

include $(MICROPYTHON_TOP)/ports/embed/embed.mk
