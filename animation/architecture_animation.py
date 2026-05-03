from manim import *


class ZeroMQNotebookArchitecture(Scene):
    def make_box(self, title, subtitle, color):
        box = RoundedRectangle(width=3.0, height=1.35, corner_radius=0.12)
        box.set_stroke(color, width=3)
        box.set_fill(color, opacity=0.10)
        title_text = Text(title, font_size=24, weight=BOLD)
        subtitle_text = Text(subtitle, font_size=15, color=GRAY_B)
        group = VGroup(box, title_text, subtitle_text)
        title_text.move_to(box.get_center() + UP * 0.20)
        subtitle_text.move_to(box.get_center() + DOWN * 0.22)
        return group

    def construct(self):
        self.camera.background_color = "#fbfbfb"

        title = Text("Mini Jupyter Notebook Clone with ZeroMQ", font_size=34, weight=BOLD)
        title.to_edge(UP)
        self.play(Write(title))

        browser = self.make_box("Browser UI", "HTML / CSS / JS", BLUE_D)
        server = self.make_box("C HTTP Server", "REST API + static files", TEAL_D)
        broker = self.make_box("ZeroMQ Broker", "ROUTER / DEALER proxy", ORANGE)
        worker = self.make_box("C Kernel Worker", "compile + run snippets", GREEN_D)
        runtime = self.make_box("Runtime Files", "generated C + output", PURPLE_D)
        status = self.make_box("PUB/SUB Status", "topic: kernel", RED_D)

        row = VGroup(browser, server, broker, worker).arrange(RIGHT, buff=0.55)
        row.move_to(UP * 0.35)
        runtime.next_to(worker, DOWN, buff=0.75)
        status.next_to(server, DOWN, buff=0.75)

        self.play(
            LaggedStart(
                FadeIn(browser, shift=UP * 0.2),
                FadeIn(server, shift=UP * 0.2),
                FadeIn(broker, shift=UP * 0.2),
                FadeIn(worker, shift=UP * 0.2),
                lag_ratio=0.18,
            )
        )

        http_arrow = Arrow(browser.get_right(), server.get_left(), buff=0.15, color=BLUE_D)
        req_arrow = Arrow(server.get_right(), broker.get_left(), buff=0.15, color=TEAL_D)
        work_arrow = Arrow(broker.get_right(), worker.get_left(), buff=0.15, color=ORANGE)
        run_arrow = Arrow(worker.get_bottom(), runtime.get_top(), buff=0.15, color=GREEN_D)
        status_arrow = Arrow(worker.get_bottom() + LEFT * 0.35, status.get_right(), buff=0.15, color=RED_D)

        labels = VGroup(
            Text("HTTP", font_size=18).next_to(http_arrow, UP, buff=0.08),
            Text("multipart RUN + JSON", font_size=15).next_to(req_arrow, UP, buff=0.08),
            Text("shared queue", font_size=16).next_to(work_arrow, UP, buff=0.08),
            Text("gcc + timeout", font_size=16).next_to(run_arrow, RIGHT, buff=0.08),
            Text("PUB/SUB", font_size=16).next_to(status_arrow, DOWN, buff=0.08),
        )

        self.play(
            Create(http_arrow),
            Create(req_arrow),
            Create(work_arrow),
            FadeIn(labels[0]),
            FadeIn(labels[1]),
            FadeIn(labels[2]),
        )
        self.play(FadeIn(runtime), Create(run_arrow), FadeIn(labels[3]))
        self.play(FadeIn(status), Create(status_arrow), FadeIn(labels[4]))

        request_dot = Dot(color=BLUE_D).move_to(browser.get_right())
        self.play(FadeIn(request_dot))
        self.play(MoveAlongPath(request_dot, http_arrow), run_time=0.8)
        self.play(MoveAlongPath(request_dot, req_arrow), run_time=0.8)
        self.play(MoveAlongPath(request_dot, work_arrow), run_time=0.8)
        self.play(MoveAlongPath(request_dot, run_arrow), run_time=0.7)

        output = Text("stdout / compile errors return as JSON", font_size=22, color=GREEN_D)
        output.to_edge(DOWN)
        self.play(Write(output))

        reply_path = VGroup(
            Arrow(runtime.get_top(), worker.get_bottom(), buff=0.15, color=GREEN_D),
            Arrow(worker.get_left(), broker.get_right(), buff=0.15, color=ORANGE),
            Arrow(broker.get_left(), server.get_right(), buff=0.15, color=TEAL_D),
            Arrow(server.get_left(), browser.get_right(), buff=0.15, color=BLUE_D),
        )
        self.play(LaggedStart(*[Create(a) for a in reply_path], lag_ratio=0.18))
        self.wait(1.5)
