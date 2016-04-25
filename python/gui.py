from gi.repository import Gtk

from gui_handler import Handler

builder = Gtk.Builder()
builder.add_from_file("glade/2ndgui.glade")

main_window = builder.get_object("main_window")
builder.connect_signals(Handler(builder))

main_window.show_all()

Gtk.main()
