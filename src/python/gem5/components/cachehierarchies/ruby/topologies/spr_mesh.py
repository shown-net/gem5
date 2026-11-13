"""Four-tile SPR Garnet candidates; coordinates are explicit hypotheses."""
from m5.objects import (
    GarnetNetwork, GarnetRouter, GarnetNetworkInterface,
    GarnetExtLink, GarnetIntLink, NetworkBridge,
)


class SPRMesh(GarnetNetwork):
    def __init__(self, ruby_system, parameters):
        super().__init__()
        self.ruby_system = ruby_system
        self.clk_domain = ruby_system.clk_domain
        self.number_of_virtual_networks = 3
        self._spr = dict(parameters)
        self._tile_rows = int(parameters["tile_rows"])
        self._tile_cols = int(parameters["tile_columns"])
        self._rows = self._tile_rows * int(parameters["tiles_per_column"])
        self._cols = self._tile_cols * int(parameters["tiles_per_row"])
        self.num_rows = self._rows
        for name in ("ni_flit_size", "vcs_per_vnet", "buffers_per_data_vc",
                     "buffers_per_ctrl_vc", "routing_algorithm"):
            setattr(self, name, parameters[name])
        self.enable_fault_model = False

    def _router_for_stop(self, stop):
        area = self._tile_rows * self._tile_cols
        tile, local = divmod(int(stop), area)
        tiles_per_row = int(self._spr["tiles_per_row"])
        if stop < 0 or tile >= tiles_per_row * int(self._spr["tiles_per_column"]):
            raise ValueError("mesh stop outside the selected tile geometry")
        row = (tile // tiles_per_row) * self._tile_rows + local // self._tile_cols
        col = (tile % tiles_per_row) * self._tile_cols + local % self._tile_cols
        return row * self._cols + col

    def _imc_stops(self):
        if self._spr["imc_stops"]:
            return list(self._spr["imc_stops"])
        area = self._tile_rows * self._tile_cols
        return [tile * area + (self._tile_rows // 2) * self._tile_cols +
                (0 if tile % 2 == 0 else self._tile_cols - 1)
                for tile in range(4)]

    def _bridge(self, link, direction, clock):
        return NetworkBridge(link=link, vtype=direction,
                             width=self.ni_flit_size,
                             cdc_latency=self._spr["cdc_latency"],
                             clk_domain=clock)

    def connectControllers(self, controllers, *, core_count, bank_count,
                           directory_count, channels):
        if self._tile_rows * self._tile_cols != 15 or self._rows * self._cols != 60:
            raise ValueError("SPR candidates require four tiles with 15 stops each")
        active = [i for i in range(60) if int(self._spr["cha_mask"]) & (1 << i)]
        if len(active) != bank_count:
            raise ValueError("CHA mask and LLC bank count disagree")
        core_stops = list(self._spr["core_stops"])
        if not core_stops:
            # CPU0 is housekeeping, CPU1 is the first business core. This
            # seed is a candidate mapping, not a recovered APIC/CHA map.
            core_stops = ([active[1], active[0]] + active[2:])[:core_count]
        if len(core_stops) != core_count:
            raise ValueError("one explicit physical stop is required per guest core")
        imcs = self._imc_stops()
        if len(imcs) != 4 or channels % 4:
            raise ValueError("four IMC endpoints must divide the subchannels evenly")
        stops = core_stops * 2 + active
        stops += [imcs[((i % channels) * 4) // channels]
                  for i in range(directory_count)]
        stops += [imcs[0]] * (len(controllers) - len(stops))
        self.routers = [GarnetRouter(router_id=i,
                                    latency=self._spr["router_latency"],
                                    clk_domain=self.clk_domain)
                        for i in range(self._rows * self._cols)]
        ext = []
        interfaces = []
        for index, (controller, stop) in enumerate(zip(controllers, stops)):
            if index >= core_count * 2 + bank_count + directory_count:
                controller.clk_domain = self.clk_domain
            endpoint_clock = controller.clk_domain
            link = GarnetExtLink(
                link_id=index, ext_node=controller,
                int_node=self.routers[self._router_for_stop(stop)],
                latency=self._spr["link_latency"],
                width=self.ni_flit_size,
                ext_cdc=endpoint_clock is not self.clk_domain,
            )
            for channel in (*link.network_links, *link.credit_links):
                channel.clk_domain = self.clk_domain
            interfaces.append(GarnetNetworkInterface(
                id=index, clk_domain=endpoint_clock))
            link.ext_net_bridge = [
                self._bridge(link.network_links[0], "OBJECT_LINK", endpoint_clock),
                self._bridge(link.network_links[1], "LINK_OBJECT", endpoint_clock)]
            link.ext_cred_bridge = [
                self._bridge(link.credit_links[0], "LINK_OBJECT", endpoint_clock),
                self._bridge(link.credit_links[1], "OBJECT_LINK", endpoint_clock)]
            link.int_net_bridge = [
                self._bridge(link.network_links[0], "LINK_OBJECT", self.clk_domain),
                self._bridge(link.network_links[1], "OBJECT_LINK", self.clk_domain)]
            link.int_cred_bridge = [
                self._bridge(link.credit_links[0], "OBJECT_LINK", self.clk_domain),
                self._bridge(link.credit_links[1], "LINK_OBJECT", self.clk_domain)]
            ext.append(link)
        self.ext_links = ext
        self.netifs = interfaces
        links = []
        directions = ((0, 1, "East", "West", 1),
                      (0, -1, "West", "East", 1),
                      (1, 0, "North", "South", 2),
                      (-1, 0, "South", "North", 2))
        for row in range(self._rows):
            for col in range(self._cols):
                for dr, dc, outport, inport, weight in directions:
                    target_row, target_col = row + dr, col + dc
                    if not (0 <= target_row < self._rows and 0 <= target_col < self._cols):
                        continue
                    cross_tile = (row // self._tile_rows != target_row // self._tile_rows or
                                  col // self._tile_cols != target_col // self._tile_cols)
                    link = GarnetIntLink(
                        link_id=len(controllers) + len(links),
                        src_node=self.routers[row * self._cols + col],
                        dst_node=self.routers[target_row * self._cols + target_col],
                        src_outport=outport, dst_inport=inport, weight=weight,
                        latency=self._spr["inter_tile_latency"] if cross_tile
                                else self._spr["link_latency"],
                        width=self.ni_flit_size)
                    link.network_link.clk_domain = self.clk_domain
                    link.credit_link.clk_domain = self.clk_domain
                    link.src_net_bridge = self._bridge(link.network_link, "OBJECT_LINK", self.clk_domain)
                    link.src_cred_bridge = self._bridge(link.credit_link, "LINK_OBJECT", self.clk_domain)
                    link.dst_net_bridge = self._bridge(link.network_link, "LINK_OBJECT", self.clk_domain)
                    link.dst_cred_bridge = self._bridge(link.credit_link, "OBJECT_LINK", self.clk_domain)
                    links.append(link)
        self.int_links = links

    def setup_buffers(self):
        # Garnet buffers are owned by VCs; there are no SimpleNetwork link
        # MessageBuffers or a second packet-level buffering implementation.
        pass
