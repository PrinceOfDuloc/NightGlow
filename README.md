**NightGlow** is a smart hallway night light designed from the ground up with a focus on efficiency, reliability, and engineering transparency. Rather than treating it as a simple LED lamp, the project explores every major subsystem—from optical design and power electronics to analog sensing and firmware architecture.

The hardware combines a high-efficiency constant-current LED driver, a mmWave motion sensor, a photodiode-based ambient light measurement circuit, and a custom control board built around an AVR microcontroller. The firmware implements adaptive brightness control, configurable operating modes, automatic standby behaviour, and non-blocking task scheduling while keeping resource usage minimal.

This repository documents the complete engineering process, including design decisions, calculations, simulations, component selection, bench measurements, firmware implementation, PCB design, and validation. The report explains not only **what** was built, but also **why** each engineering decision was made and what alternatives were considered.

📖 **Start here:** [**Design Walkthrough**](https://princeofduloc.github.io/NightGlow/Design%20Walkthrough/NightGlow%20Design%20Walkthrough.html)

Whether you're interested in embedded firmware, analog circuit design, power electronics, PCB development, or simply enjoy seeing how a product is engineered from first principles, the design report is intended to provide a complete and reproducible walkthrough of the project.