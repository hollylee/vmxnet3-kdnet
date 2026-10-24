# Draft feedback: KDNET contact address rejects external senders

The `MicrosoftDocs/windows-driver-docs` repository has GitHub Issues turned
off. The page uses Microsoft Learn's own feedback instead. Post the short
version below through the page's **Feedback** control (top of the page), or
"Was this page helpful?" at the bottom, with the negative option.

## Short version (for the page feedback box)

The page asks developers to email kdnet@microsoft.com, but external mail to
it bounces: postmaster@microsoft.com reports that teamtm@microsoft.com only
accepts messages from its organization or allowed senders. Please give a
contact or channel that external KDNET module developers can use (for
example for signing questions or DBG2 subtype reservations). Also, the page
requires kdnetextensibility.h, which isn't in the WDK (10.0.26100): where
can developers get it?

## Full version (if a longer form is available)

**Title:** KDNET extensibility guide: contact address kdnet@microsoft.com rejects external senders

---

**Page:** [Develop KDNET Transport Extensibility Modules](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/how-to-develop-kdnet-extensibility-modules)

The page asks developers to email kdnet@microsoft.com, for example to request
additional import routines or to reserve serial/USB DBG2 subtypes. Mail from
outside Microsoft to that address is rejected. The bounce from
postmaster@microsoft.com says the message to teamtm@microsoft.com couldn't be
delivered because the group only accepts messages from people in its
organization or on its allowed senders list.

So external developers, who are the audience for this guide, have no working
way to reach the KDNET team through the documented channel.

**Request:** please update the page with a contact route that accepts
external mail, or name the intended channel instead (for example Partner
Center support, or Microsoft Q&A with a specific tag).

**Context:** I'm developing a KDNET extensibility module for VMware's vmxnet3
NIC (PCI 15AD:07B0) on Windows 11 ARM64, which has no inbox kd_02_15ad.dll. I
wanted to ask how third-party modules can be signed: on 25H2, winload loads
the module only when driver signature enforcement is disabled for the boot,
not with test signing.

**Related gap on the same page:** the page says modules must include
kdnetextensibility.h, but that header isn't in the WDK (checked 10.0.26100).
It would help to say where module developers can get it.
