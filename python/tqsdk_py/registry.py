"""Internal strategy registry.

Concrete strategies register themselves with a decorator when their module is
imported. The framework only knows names, defaults and the generic Strategy
constructor contract; it never imports a concrete strategy directly.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Iterable, Mapping, TypeVar

from .models import ContractSpec
from .strategy import Strategy

StrategyType = TypeVar("StrategyType", bound=type[Strategy])


class StrategyRegistryError(RuntimeError):
    pass


class UnknownStrategyError(StrategyRegistryError):
    pass


@dataclass(frozen=True)
class StrategyDefinition:
    name: str
    strategy_class: type[Strategy]
    defaults: dict[str, Any]
    aliases: tuple[str, ...] = ()
    description: str = ""

    def create(
        self,
        *,
        spec: ContractSpec,
        symbol: str = "",
        duration_ns: int = 86_400_000_000_000,
        params: Mapping[str, Any] | None = None,
    ) -> Strategy:
        values = dict(self.defaults)
        values.update(params or {})
        values["symbol"] = symbol
        values["duration_ns"] = duration_ns
        return self.strategy_class(spec=spec, **values)


class StrategyRegistry:
    def __init__(self) -> None:
        self._definitions: dict[str, StrategyDefinition] = {}

    def register(
        self,
        name: str,
        strategy_class: type[Strategy],
        *,
        aliases: Iterable[str] = (),
        defaults: Mapping[str, Any] | None = None,
        description: str = "",
    ) -> StrategyDefinition:
        key = name.strip()
        if not key:
            raise StrategyRegistryError("strategy name must not be empty")
        existing = self._definitions.get(key)
        if existing is not None and existing.strategy_class is not strategy_class:
            raise StrategyRegistryError(f"duplicate strategy name: {key}")
        definition = StrategyDefinition(
            name=key,
            strategy_class=strategy_class,
            defaults=dict(defaults or {}),
            aliases=tuple(aliases),
            description=description,
        )
        self._definitions[key] = definition
        for alias in aliases:
            alias_key = alias.strip()
            if alias_key:
                self._definitions[alias_key] = definition
        return definition

    def resolve(self, name: str) -> StrategyDefinition:
        definition = self._definitions.get(name)
        if definition is None:
            available = ", ".join(sorted(self.names()))
            raise UnknownStrategyError(
                f"unknown strategy '{name}'; available: {available or '<none>'}"
            )
        return definition

    def names(self) -> list[str]:
        return sorted(self._definitions)

    def definitions(self) -> list[StrategyDefinition]:
        unique: dict[int, StrategyDefinition] = {}
        for definition in self._definitions.values():
            unique[id(definition)] = definition
        return sorted(unique.values(), key=lambda item: item.name)

    def create(
        self,
        name: str,
        *,
        spec: ContractSpec,
        symbol: str = "",
        duration_ns: int = 86_400_000_000_000,
        params: Mapping[str, Any] | None = None,
    ) -> Strategy:
        return self.resolve(name).create(
            spec=spec,
            symbol=symbol,
            duration_ns=duration_ns,
            params=params,
        )


REGISTRY = StrategyRegistry()


def register_strategy(
    name: str,
    *,
    aliases: Iterable[str] = (),
    defaults: Mapping[str, Any] | None = None,
    description: str = "",
) -> Callable[[StrategyType], StrategyType]:
    """Class decorator used by strategy modules.

    Example:
        @register_strategy("ema_donchian", defaults={"ema_period": 60})
        class EmaDonchianStrategy(Strategy):
            def __init__(self, spec, symbol="", duration_ns=..., ema_period=60):
                ...
    """

    def decorator(cls: StrategyType) -> StrategyType:
        REGISTRY.register(
            name,
            cls,
            aliases=aliases,
            defaults=defaults,
            description=description,
        )
        return cls

    return decorator
